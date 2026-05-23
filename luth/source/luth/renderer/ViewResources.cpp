#include "luthpch.h"
#include "luth/renderer/RenderPipeline.h"
#include "luth/renderer/subsystems/GlobalSubsystem.h"
#include "luth/renderer/subsystems/GTAOSubsystem.h"
#include "luth/renderer/subsystems/PostProcessSubsystem.h"
#include "luth/renderer/subsystems/EditorOverlaysSubsystem.h"
#include "luth/scene/systems/RenderingSystem.h"
#include "luth/renderer/backend/vulkan/VulkanContext.h"
#include "luth/renderer/backend/vulkan/VulkanTexture.h"
#include "luth/renderer/backend/vulkan/VulkanBuffer.h"
#include "luth/renderer/resources/Texture.h"
#include "luth/renderer/settings/GTAOSettings.h"
#include "luth/renderer/settings/PostProcessSettings.h"
#include "luth/core/diagnostics/Log.h"

namespace Luth
{
    // Per-view pool: cycled sets allocate MAX_FRAMES_IN_FLIGHT instances each.
    static constexpr u32 k_ViewPoolMaxSets              = 48;
    static constexpr u32 k_ViewPoolUniformBufferCount   = 32;
    static constexpr u32 k_ViewPoolStorageImageCount    = 8;
    static constexpr u32 k_ViewPoolStorageBufferCount   = 48;  // Set 3 + cluster build + light assign cycled sets
    static constexpr u32 k_ViewPoolCombinedSamplerCount = 64;

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
        else if (vr.width != newW || vr.height != newH)
        {
            const u32 halfW = std::max(newW / 2, 1u);
            const u32 halfH = std::max(newH / 2, 1u);
            RecreateViewTextures(vr, halfW, halfH);
            m_PostProcess.WriteView(vr, targets);
            m_GTAO.WriteView(vr, targets);
            m_EditorOverlays.WriteOutlineView(vr, targets);
            m_EditorOverlays.WriteGridView(vr, targets);
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

        const u32 halfW = std::max(targets.GetSceneColor()->GetWidth()  / 2, 1u);
        const u32 halfH = std::max(targets.GetSceneColor()->GetHeight() / 2, 1u);
        RecreateViewTextures(vr, halfW, halfH);

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
            vkAllocateDescriptorSets(device, &ai, outArr.data());
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

        m_PostProcess.WriteView(vr, targets);
        m_GTAO.WriteView(vr, targets);
        m_EditorOverlays.WriteOutlineView(vr, targets);
        m_EditorOverlays.WriteGridView(vr, targets);
        m_Lighting.WriteShadowView(vr);
        m_Lighting.WriteClusterVizView(vr, targets);
        // Global writes last — reads vr.gtaoFinal view that GTAO writes set up.
        m_Global.WriteView(vr, MakeGlobalCtx(*this, vr));
    }

    void RenderPipeline::RecreateViewTextures(ViewResources& vr, u32 halfW, u32 halfH)
    {
        vr.bloomA = Texture::Create(halfW, halfH, TextureFormat::RGBA16F);
        vr.bloomB = Texture::Create(halfW, halfH, TextureFormat::RGBA16F);

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
