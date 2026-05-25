#include "luthpch.h"
#include "luth/renderer/RenderPipeline.h"
#include "luth/renderer/subsystems/GlobalSubsystem.h"
#include "luth/renderer/subsystems/GTAOSubsystem.h"
#include "luth/renderer/subsystems/PostProcessSubsystem.h"
#include "luth/renderer/subsystems/EditorOverlaysSubsystem.h"
#include "luth/renderer/subsystems/VolumetricSubsystem.h"
#include "luth/scene/systems/RenderingSystem.h"
#include "luth/renderer/backend/vulkan/VulkanContext.h"
#include "luth/renderer/backend/vulkan/VulkanTexture.h"
#include "luth/renderer/backend/vulkan/VulkanBuffer.h"
#include "luth/renderer/resources/Texture.h"
#include "luth/renderer/settings/GTAOSettings.h"
#include "luth/renderer/settings/PostProcessSettings.h"
#include "luth/renderer/settings/VolumetricSettings.h"
#include "luth/core/diagnostics/Log.h"

namespace Luth
{
    // Per-view pool: cycled sets allocate MAX_FRAMES_IN_FLIGHT instances each. Capacity bumped on
    // every subsystem addition — silent vkAllocateDescriptorSets failure on overflow returns
    // VK_NULL_HANDLE handles and skips the draw with no log (volumetric viz v3.0.6 lesson).
    // Bump generously; pool memory is cheap.
    static constexpr u32 k_ViewPoolMaxSets              = 96;
    static constexpr u32 k_ViewPoolUniformBufferCount   = 48;
    static constexpr u32 k_ViewPoolStorageImageCount    = 32;  // GTAO + volumetric atlases (cycled)
    static constexpr u32 k_ViewPoolStorageBufferCount   = 80;  // Set 3 + cluster + assign + volumetric
    static constexpr u32 k_ViewPoolCombinedSamplerCount = 128;

    namespace {
        // Build the per-view Set 0 write context from RP-side state. invariant:
        // GTAO textures must already exist (RecreateViewTextures runs before).
        GlobalViewWriteContext MakeGlobalCtx(const RenderPipeline& rp, const ViewResources& vr)
        {
            const auto& lighting = rp.GetLighting();
            GlobalViewWriteContext ctx{};
            ctx.haveIBL          = lighting.IsIBLReady();
            ctx.iblSampler       = lighting.GetIBLSampler();
            ctx.gtaoSampler      = rp.GetGTAO().GetSampler();
            if (ctx.haveIBL)
            {
                ctx.irradianceView  = std::static_pointer_cast<VKTexture>(lighting.GetIrradianceMap())->GetImageView();
                ctx.prefilteredView = std::static_pointer_cast<VKTexture>(lighting.GetPrefilteredMap())->GetImageView();
                ctx.brdfView        = std::static_pointer_cast<VKTexture>(lighting.GetBRDFLut())->GetImageView();
            }
            ctx.gtaoFinalView = vr.gtaoFinal
                ? std::static_pointer_cast<VKTexture>(vr.gtaoFinal)->GetImageView()
                : VK_NULL_HANDLE;
            return ctx;
        }
    }

    ViewResources& RenderPipeline::EnsureViewResources(FrameTargets& targets)
    {
        auto [it, inserted] = m_ViewResources.try_emplace(&targets);
        ViewResources& vr = it->second;

        if (!targets.GetSceneColor())
            return vr;

        const u32 newW = targets.GetSceneColor()->GetWidth();
        const u32 newH = targets.GetSceneColor()->GetHeight();

        if (inserted)
        {
            // Mint a fresh identity token. Survives resize, dies with
            // ReleaseViewResources — replay's HasViewResources(t,id) check
            // detects FrameTargets-pointer reuse after a panel close.
            static std::atomic<u64> s_NextId{ 1 };
            vr.id = s_NextId.fetch_add(1, std::memory_order_relaxed);
            AllocateViewResources(vr, targets);
        }
        else if (vr.width != newW || vr.height != newH ||
                 vr.volQualityCached != static_cast<u32>(m_System.GetVolumetricSettings().quality))
        {
            const u32 halfW = std::max(newW / 2, 1u);
            const u32 halfH = std::max(newH / 2, 1u);
            RecreateViewTextures(vr, newW, newH, halfW, halfH);
            m_PostProcess.WriteView(vr, targets);
            m_GTAO.WriteView(vr, targets);
            m_EditorOverlays.WriteOutlineView(vr, targets);
            m_EditorOverlays.WriteGridView(vr, targets);
            // sceneDepth + atlases are per-view + recreated on resize/quality change, so re-bind
            // the inject/integrate/composite/viz descriptors that reference them.
            m_Volumetric.WriteInjectDensityView(vr);
            m_Volumetric.WriteInjectScatterView(vr);
            m_Volumetric.WriteIntegrateView(vr);
            m_Volumetric.WriteResolveView(vr);
            m_Volumetric.WriteCompositeView(vr, targets);
            m_Volumetric.WriteVizView(vr, targets);
            // Set 0 bindings 1-4 reference the (re)created IBL + GTAO textures.
            m_Global.WriteView(vr, MakeGlobalCtx(*this, vr));
        }

        vr.width  = newW;
        vr.height = newH;
        return vr;
    }

    void RenderPipeline::ReleaseViewResources(FrameTargets& targets)
    {
        auto it = m_ViewResources.find(&targets);
        if (it == m_ViewResources.end()) return;
        DestroyViewResources(it->second);
        m_ViewResources.erase(it);
    }

    bool RenderPipeline::HasViewResources(FrameTargets* targets, u64 expectedId) const
    {
        if (!targets) return false;
        auto it = m_ViewResources.find(targets);
        return it != m_ViewResources.end() && it->second.id == expectedId;
    }

    ViewResources* RenderPipeline::GetViewResources(FrameTargets* targets)
    {
        if (!targets) return nullptr;
        auto it = m_ViewResources.find(targets);
        return it == m_ViewResources.end() ? nullptr : &it->second;
    }

    const ViewResources* RenderPipeline::GetViewResources(FrameTargets* targets) const
    {
        if (!targets) return nullptr;
        auto it = m_ViewResources.find(targets);
        return it == m_ViewResources.end() ? nullptr : &it->second;
    }

    void RenderPipeline::AllocateViewResources(ViewResources& vr, FrameTargets& targets)
    {
        VkDevice device = VulkanContext::Get().GetDevice();

        VkDescriptorPoolSize poolSizes[4] = {};
        poolSizes[0].type            = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        poolSizes[0].descriptorCount = k_ViewPoolUniformBufferCount;
        poolSizes[1].type            = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        poolSizes[1].descriptorCount = k_ViewPoolStorageImageCount;
        poolSizes[2].type            = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        poolSizes[2].descriptorCount = k_ViewPoolCombinedSamplerCount;
        poolSizes[3].type            = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        poolSizes[3].descriptorCount = k_ViewPoolStorageBufferCount;

        VkDescriptorPoolCreateInfo poolInfo{ VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
        poolInfo.flags         = VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT;
        poolInfo.maxSets       = k_ViewPoolMaxSets;
        poolInfo.poolSizeCount = 4;
        poolInfo.pPoolSizes    = poolSizes;
        vkCreateDescriptorPool(device, &poolInfo, nullptr, &vr.descPool);

        // Set 0 UBO bindings (0 + 5) and Grid set binding 0 are written per render-stage
        // by UpdateGlobalUniforms / UpdateGTAOUBO; nothing to allocate up front.

        const u32 fullW = targets.GetSceneColor()->GetWidth();
        const u32 fullH = targets.GetSceneColor()->GetHeight();
        const u32 halfW = std::max(fullW / 2, 1u);
        const u32 halfH = std::max(fullH / 2, 1u);
        RecreateViewTextures(vr, fullW, fullH, halfW, halfH);

        auto allocSingle = [&](VkDescriptorSetLayout layout, VkDescriptorSet& outSet, const char* tag) {
            if (layout == VK_NULL_HANDLE) return;
            VkDescriptorSetAllocateInfo ai{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
            ai.descriptorPool     = vr.descPool;
            ai.descriptorSetCount = 1;
            ai.pSetLayouts        = &layout;
            vkAllocateDescriptorSets(device, &ai, &outSet);
            VulkanContext::SetDebugName(outSet, tag);
        };
        auto allocCycled = [&](VkDescriptorSetLayout layout,
                               std::array<VkDescriptorSet, MAX_FRAMES_IN_FLIGHT>& outArr,
                               const char* tagPrefix) {
            if (layout == VK_NULL_HANDLE) return;
            VkDescriptorSetLayout layouts[MAX_FRAMES_IN_FLIGHT];
            for (u32 i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i) layouts[i] = layout;
            VkDescriptorSetAllocateInfo ai{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
            ai.descriptorPool     = vr.descPool;
            ai.descriptorSetCount = MAX_FRAMES_IN_FLIGHT;
            ai.pSetLayouts        = layouts;
            VkResult result = vkAllocateDescriptorSets(device, &ai, outArr.data());
            if (result != VK_SUCCESS)
            {
                LH_CORE_ERROR("ViewResources: allocCycled '{}' failed (VkResult {}); bump pool sizes", tagPrefix, (int)result);
                for (u32 i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i) outArr[i] = VK_NULL_HANDLE;
                return;
            }
            for (u32 i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i)
            {
                char name[64]; std::snprintf(name, sizeof(name), "%s.Slot%u", tagPrefix, i);
                VulkanContext::SetDebugName(outArr[i], name);
            }
        };

        const VkDescriptorSetLayout ppLayout = m_PostProcess.GetDescSetLayout();
        allocCycled(m_Global.GetSetLayout(),             vr.globalDescriptorSet,  "View.Global");
        allocCycled(ppLayout,                            vr.bloomExtractDescSet,  "View.BloomExtract");
        allocCycled(ppLayout,                            vr.bloomBlurHDescSet,    "View.BloomBlurH");
        allocCycled(ppLayout,                            vr.bloomBlurVDescSet,    "View.BloomBlurV");
        allocCycled(ppLayout,                            vr.compositeDescSet,     "View.Composite");
        allocSingle(m_GTAO.GetPrefilterLayout(),         vr.gtaoPrefilterDescSet, "View.GTAOPrefilter");
        allocCycled(m_GTAO.GetMainLayout(),              vr.gtaoMainDescSet,      "View.GTAOMain");
        allocSingle(m_GTAO.GetDenoiseLayout(),           vr.gtaoDenoiseDescSet,   "View.GTAODenoise");
        allocSingle(m_EditorOverlays.GetOutlineLayout(), vr.outlineDescSet,       "View.Outline");
        allocCycled(m_EditorOverlays.GetGridLayout(),    vr.gridDescSet,          "View.Grid");
        allocSingle(m_PostProcess.GetSlimVizDescSetLayout(), vr.slimVizDescSet,   "View.SlimViz");
        allocCycled(m_Lighting.GetSetLayout(),           vr.lightDescSet,         "View.Light");
        allocCycled(m_Lighting.GetClusterBuildLayout(),  vr.clusterBuildDescSet,  "View.ClusterBuild");
        allocCycled(m_Lighting.GetLightAssignLayout(),   vr.lightAssignDescSet,   "View.LightAssign");
        allocSingle(m_Lighting.GetClusterVizLayout(),    vr.clusterVizDescSet,    "View.ClusterViz");
        allocCycled(m_Volumetric.GetInjectDensityLayout(), vr.volInjectDensityDescSet, "View.VolInjectDensity");
        allocCycled(m_Volumetric.GetInjectScatterLayout(), vr.volInjectScatterDescSet, "View.VolInjectScatter");
        allocCycled(m_Volumetric.GetIntegrateLayout(),     vr.volIntegrateDescSet,     "View.VolIntegrate");
        allocCycled(m_Volumetric.GetResolveLayout(),     vr.volResolveDescSet,    "View.VolResolve");
        allocCycled(m_Volumetric.GetCompositeLayout(),   vr.volCompositeDescSet,  "View.VolComposite");
        allocCycled(m_Volumetric.GetVizLayout(),         vr.volVizDescSet,        "View.VolViz");
        allocCycled(m_PostProcess.GetTaaResolveDescSetLayout(), vr.taaResolveDescSet, "View.TaaResolve");

        m_PostProcess.WriteView(vr, targets);
        m_GTAO.WriteView(vr, targets);
        m_EditorOverlays.WriteOutlineView(vr, targets);
        m_EditorOverlays.WriteGridView(vr, targets);
        m_Lighting.WriteShadowView(vr);
        m_Lighting.WriteClusterVizView(vr, targets);
        m_Volumetric.WriteInjectDensityView(vr);
        m_Volumetric.WriteInjectScatterView(vr);
        m_Volumetric.WriteIntegrateView(vr);
        m_Volumetric.WriteResolveView(vr);
        m_Volumetric.WriteCompositeView(vr, targets);
        m_Volumetric.WriteVizView(vr, targets);
        // Global writes last — reads vr.gtaoFinal view that GTAO writes set up.
        m_Global.WriteView(vr, MakeGlobalCtx(*this, vr));
    }

    void RenderPipeline::RecreateViewTextures(ViewResources& vr, u32 fullW, u32 fullH, u32 halfW, u32 halfH)
    {
        vr.bloomA = Texture::Create(halfW, halfH, TextureFormat::RGBA16F);
        vr.bloomB = Texture::Create(halfW, halfH, TextureFormat::RGBA16F);

        // TAA history (Karis14 YCoCg-clip recipe) — viewport-sized RGBA16F HDR. Persistent across
        // frames; ping-pong via frameAbs parity. SAMPLED for the resolve's history read; COLOR
        // attachment for the resolve's write. Frame 0 settles via off-screen UV rejection in
        // the resolve shader (motion vectors land outside [0,1] when prevViewProj is identity).
        vr.taaHistoryA = Texture::Create(fullW, fullH, TextureFormat::RGBA16F);
        vr.taaHistoryB = Texture::Create(fullW, fullH, TextureFormat::RGBA16F);

        auto makeStorage = [&](TextureFormat fmt) {
            return std::make_shared<VKTexture>(
                halfW, halfH, fmt,
                /*arrayLayers*/ 1, /*createFlags*/ 0u, /*mipLevels*/ 1,
                VK_IMAGE_USAGE_STORAGE_BIT);
        };
        vr.gtaoLinearDepth = makeStorage(TextureFormat::R32_Float);
        vr.gtaoRawAO       = makeStorage(TextureFormat::R8);
        vr.gtaoEdges       = makeStorage(TextureFormat::R8);
        vr.gtaoFinal       = makeStorage(TextureFormat::R8);

        // Volumetric fog atlas dims from current quality preset (Low / Medium / High). View-aligned
        // but dimensions are independent of viewport pixels — they don't scale with halfW/halfH.
        // Cached on vr so EnsureViewResources can detect runtime quality changes.
        const auto quality = m_System.GetVolumetricSettings().quality;
        const auto dims    = Volumetric::GetAtlasDims(quality);
        vr.volDimX = dims.x; vr.volDimY = dims.y; vr.volDimZ = dims.z;
        vr.volQualityCached = static_cast<u32>(quality);
        auto makeVolume = [&dims](TextureFormat fmt) {
            return std::make_shared<VKTexture>(dims.x, dims.y, dims.z, fmt, VK_IMAGE_USAGE_STORAGE_BIT);
        };
        vr.volDensity          = makeVolume(TextureFormat::RGBA16F);
        vr.volInScatter        = makeVolume(TextureFormat::RGBA16F);
        vr.volInScatterHistA   = makeVolume(TextureFormat::RGBA16F);
        vr.volInScatterHistB   = makeVolume(TextureFormat::RGBA16F);

        // Bootstrap clear: freshly-allocated VMA storage images have UNDEFINED layout and undefined
        // pixel content. The resolve pass samples volInScatterHist{A,B} on frame 0; without this
        // clear the first sample is NaN-prone garbage. One-shot submit per view-resize only.
        VkImage clearTargets[3] = {
            std::static_pointer_cast<VKTexture>(vr.volInScatter)->GetImage(),
            std::static_pointer_cast<VKTexture>(vr.volInScatterHistA)->GetImage(),
            std::static_pointer_cast<VKTexture>(vr.volInScatterHistB)->GetImage(),
        };
        VulkanContext::Get().ImmediateSubmit([&](VkCommandBuffer cmd) {
            VkImageMemoryBarrier toDst[3]{};
            for (u32 i = 0; i < 3; ++i)
            {
                toDst[i].sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
                toDst[i].oldLayout           = VK_IMAGE_LAYOUT_UNDEFINED;
                toDst[i].newLayout           = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
                toDst[i].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                toDst[i].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                toDst[i].image               = clearTargets[i];
                toDst[i].subresourceRange    = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
                toDst[i].srcAccessMask       = 0;
                toDst[i].dstAccessMask       = VK_ACCESS_TRANSFER_WRITE_BIT;
            }
            vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                 0, 0, nullptr, 0, nullptr, 3, toDst);

            VkClearColorValue zero{ { 0.0f, 0.0f, 0.0f, 0.0f } };
            VkImageSubresourceRange range{ VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
            for (u32 i = 0; i < 3; ++i)
                vkCmdClearColorImage(cmd, clearTargets[i], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                     &zero, 1, &range);

            VkImageMemoryBarrier toGen[3]{};
            for (u32 i = 0; i < 3; ++i)
            {
                toGen[i].sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
                toGen[i].oldLayout           = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
                toGen[i].newLayout           = VK_IMAGE_LAYOUT_GENERAL;
                toGen[i].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                toGen[i].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                toGen[i].image               = clearTargets[i];
                toGen[i].subresourceRange    = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
                toGen[i].srcAccessMask       = VK_ACCESS_TRANSFER_WRITE_BIT;
                toGen[i].dstAccessMask       = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
            }
            vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                 0, 0, nullptr, 0, nullptr, 3, toGen);
        });
    }

    void RenderPipeline::DestroyViewResources(ViewResources& vr)
    {
        // Pool destruction frees every descriptor set allocated from it.
        vr.bloomA.reset();
        vr.bloomB.reset();
        vr.gtaoLinearDepth.reset();
        vr.gtaoRawAO.reset();
        vr.gtaoEdges.reset();
        vr.gtaoFinal.reset();
        vr.volDensity.reset();
        vr.volInScatter.reset();
        vr.volInScatterHistA.reset();
        vr.volInScatterHistB.reset();
        vr.taaHistoryA.reset();
        vr.taaHistoryB.reset();

        if (vr.descPool != VK_NULL_HANDLE)
        {
            VulkanContext::Get().PushDeletion([pool = vr.descPool]() {
                vkDestroyDescriptorPool(VulkanContext::Get().GetDevice(), pool, nullptr);
            });
            vr.descPool = VK_NULL_HANDLE;
        }
        vr = {};
    }
}
