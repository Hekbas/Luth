#include "luthpch.h"
#include "luth/renderer/RenderPipeline.h"
#include "luth/renderer/subsystems/GlobalSubsystem.h"
#include "luth/renderer/subsystems/GTAOSubsystem.h"
#include "luth/renderer/subsystems/PostProcessSubsystem.h"
#include "luth/renderer/subsystems/EditorOverlaysSubsystem.h"
#include "luth/renderer/subsystems/VolumetricSubsystem.h"
#include "luth/renderer/subsystems/RtSubsystem.h"
#include "luth/scene/systems/RenderingSystem.h"
#include "luth/renderer/backend/vulkan/VulkanContext.h"
#include "luth/renderer/backend/vulkan/VulkanTexture.h"
#include "luth/renderer/backend/vulkan/VulkanBuffer.h"
#include "luth/renderer/resources/Texture.h"
#include "luth/renderer/settings/GTAOSettings.h"
#include "luth/renderer/settings/PostProcessSettings.h"
#include "luth/renderer/settings/VolumetricSettings.h"
#include "luth/memory/GPUTaggedPageAllocator.h"
#include "luth/core/diagnostics/Log.h"

namespace Luth
{
    // Per-view pool: cycled sets allocate MAX_FRAMES_IN_FLIGHT instances each. Capacity bumped on
    // every subsystem addition — silent vkAllocateDescriptorSets failure on overflow returns
    // VK_NULL_HANDLE handles and skips the draw with no log. Bump generously; pool memory is cheap.
    static constexpr u32 k_ViewPoolMaxSets              = 205;  // + DiSpecular SVGF ×7 (#154) + GI upscale + DI upscale ×2 + refl upscale + bloom pyramid sets
    static constexpr u32 k_ViewPoolUniformBufferCount   = 48;
    static constexpr u32 k_ViewPoolStorageImageCount    = 248;  // + DiSpecular SVGF + restir Set 2 b8 (#154) + GI upscale b3 + DI upscale ×2 + refl upscale b3 + bloom pyramid mips
    static constexpr u32 k_ViewPoolStorageBufferCount   = 126;  // + Transparency b2 OIT nodes ×3
    static constexpr u32 k_ViewPoolCombinedSamplerCount = 298;  // + DiSpecular SVGF + restir Set 2 b7 (#154) + GI upscale b0-b2 + DI upscale ×2 b0-b2 + refl upscale b0-b2
    static constexpr u32 k_ViewPoolAccelStructCount     = 8;   // Set 0 binding 6 (TLAS) cycled per frame

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
                 vr.volQualityCached != static_cast<u32>(m_System.GetVolumetricSettings().quality) ||
                 vr.oitLayersCached != m_System.GetTransparencySettings().avgLayersBudget ||
                 vr.giHalfCached != (m_System.GetRestirGiSettings().halfResolution ? 1u : 0u) ||
                 vr.diHalfCached != (m_System.GetRestirSettings().halfResolution ? 1u : 0u) ||
                 vr.reflHalfCached != (m_System.GetReflectionsSettings().halfResolution ? 1u : 0u))
        {
            const u32 halfW = std::max(newW / 2, 1u);
            const u32 halfH = std::max(newH / 2, 1u);
            RecreateViewTextures(vr, newW, newH, halfW, halfH);
            m_PostProcess.WriteView(vr, targets);
            m_PostProcess.WriteBloomView(vr);
            m_PostProcess.WriteTaaResolveView(vr, targets);
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
            m_Transparency.WriteOitView(vr);        // re-bind Set 6 b1/b2 + resolve set to the new heads image + pool
            m_Rt.WriteShadowPassView(vr, targets);  // re-bind binding 2 (mask storage) to the new viewport-sized image
            m_Restir.WriteView(vr, targets);        // re-bind Set 2 depth/normal + reservoir + new DI image
            m_RestirGi.WriteView(vr, targets);      // re-bind GI Set 2 depth/normal + reservoir + new GI image
            m_RestirGi.WriteReservoirVizView(vr, targets);  // re-bind GI reservoir-viz depth + spatial reservoir
            m_RestirGi.WriteUpscaleView(vr, targets);       // re-bind GI upscale half-input + full output
            m_Restir.WriteUpscaleView(vr, targets);         // re-bind DI diffuse + specular upscale sets
            m_PathTrace.WriteView(vr);              // re-bind PT accumulator + display image (recreated on resize)
            m_Reflections.WriteView(vr, targets);   // re-bind reflection output + slim G-buffer samplers
            m_Reflections.WriteUpscaleView(vr, targets);    // re-bind refl upscale half-input + full output
            m_Denoise->WriteView(vr, targets);      // re-bind DI SVGF inputs + output to the new images
            m_DenoiseGi->WriteView(vr, targets);    // re-bind GI SVGF inputs + output to the new images
            m_DenoiseRefl->WriteView(vr, targets);  // re-bind specular SVGF inputs + output to the new images
            m_DenoiseDiSpec->WriteView(vr, targets);// re-bind ReSTIR-DI specular SVGF (#154)
            m_Lighting.WriteShadowView(vr);         // re-bind Set 3 b4 sun mask + b5 denoised DI + b6 denoised GI
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

        VkDescriptorPoolSize poolSizes[5] = {};
        poolSizes[0].type            = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        poolSizes[0].descriptorCount = k_ViewPoolUniformBufferCount;
        poolSizes[1].type            = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        poolSizes[1].descriptorCount = k_ViewPoolStorageImageCount;
        poolSizes[2].type            = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        poolSizes[2].descriptorCount = k_ViewPoolCombinedSamplerCount;
        poolSizes[3].type            = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        poolSizes[3].descriptorCount = k_ViewPoolStorageBufferCount;
        poolSizes[4].type            = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
        poolSizes[4].descriptorCount = k_ViewPoolAccelStructCount;

        VkDescriptorPoolCreateInfo poolInfo{ VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
        poolInfo.flags         = VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT;
        poolInfo.maxSets       = k_ViewPoolMaxSets;
        poolInfo.poolSizeCount = 5;
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

        const VkDescriptorSetLayout ppLayout    = m_PostProcess.GetDescSetLayout();
        const VkDescriptorSetLayout bloomLayout = m_PostProcess.GetBloomComputeLayout();
        allocCycled(m_Global.GetSetLayout(),             vr.globalDescriptorSet,   "View.Global");
        allocCycled(bloomLayout,                         vr.bloomPrefilterDescSet, "View.BloomPrefilter");
        for (u32 i = 0; i < ViewResources::kBloomMipCount - 1; ++i)
        {
            char tag[32];
            std::snprintf(tag, sizeof(tag), "View.BloomDown%u", i);
            allocSingle(bloomLayout, vr.bloomDownDescSet[i], tag);
            std::snprintf(tag, sizeof(tag), "View.BloomUp%u", i);
            allocSingle(bloomLayout, vr.bloomUpDescSet[i], tag);
        }
        allocCycled(ppLayout,                            vr.compositeDescSet,      "View.Composite");
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
        allocCycled(m_Transparency.GetSetLayout(),       vr.transparentDescSet,   "View.Transparent");
        allocCycled(m_PostProcess.GetTaaResolveDescSetLayout(), vr.taaResolveDescSet, "View.TaaResolve");
        allocSingle(m_Transparency.GetResolveSetLayout(), vr.oitResolveDescSet,   "View.OitResolve");
        allocCycled(m_Rt.GetShadowPassLayout(),          vr.rtShadowPassDescSet,  "View.RtShadowPass");
        allocCycled(m_Restir.GetSetLayout(),             vr.restirDescSet,        "View.Restir");
        allocCycled(m_RestirGi.GetSetLayout(),           vr.restirGiDescSet,      "View.RestirGi");
        allocSingle(m_RestirGi.GetReservoirVizLayout(),  vr.giReservoirVizDescSet,"View.GiReservoirViz");
        allocSingle(m_RestirGi.GetUpscaleLayout(),       vr.giUpscaleDescSet,     "View.GiUpscale");
        allocSingle(m_Restir.GetUpscaleLayout(),         vr.diUpscaleDescSet,     "View.DiUpscale");
        allocSingle(m_Restir.GetUpscaleLayout(),         vr.diSpecUpscaleDescSet, "View.DiSpecUpscale");
        allocSingle(m_PathTrace.GetSetLayout(),          vr.ptDescSet,            "View.PathTrace");
        allocSingle(m_Reflections.GetSetLayout(),        vr.reflDescSet,          "View.Reflections");
        allocSingle(m_Reflections.GetUpscaleLayout(),    vr.reflUpscaleDescSet,   "View.ReflUpscale");
        m_Denoise->AllocateViewSets(vr);
        m_DenoiseGi->AllocateViewSets(vr);
        m_DenoiseRefl->AllocateViewSets(vr);
        m_DenoiseDiSpec->AllocateViewSets(vr);

        m_PostProcess.WriteView(vr, targets);
        m_PostProcess.WriteBloomView(vr);
        m_PostProcess.WriteTaaResolveView(vr, targets);
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
        m_Transparency.WriteOitView(vr);
        m_Rt.WriteShadowPassView(vr, targets);
        m_Restir.WriteView(vr, targets);
        m_RestirGi.WriteView(vr, targets);
        m_RestirGi.WriteReservoirVizView(vr, targets);
        m_RestirGi.WriteUpscaleView(vr, targets);
        m_Restir.WriteUpscaleView(vr, targets);
        m_PathTrace.WriteView(vr);
        m_Reflections.WriteView(vr, targets);
        m_Reflections.WriteUpscaleView(vr, targets);    // bind refl upscale half-input + full output
        m_Denoise->WriteView(vr, targets);
        m_DenoiseGi->WriteView(vr, targets);
        m_DenoiseRefl->WriteView(vr, targets);
        m_DenoiseDiSpec->WriteView(vr, targets);
        // Global writes last — reads vr.gtaoFinal view that GTAO writes set up.
        m_Global.WriteView(vr, MakeGlobalCtx(*this, vr));
    }

    void RenderPipeline::RecreateViewTextures(ViewResources& vr, u32 fullW, u32 fullH, u32 halfW, u32 halfH)
    {
        // Half-res GI (RestirGiSettings::halfResolution): GI reservoirs + restirGiDI + svgfGi* history
        // allocate at half extent; svgfGiDenoised stays full (the bilateral-upscale output). giHalfCached
        // lets EnsureViewResources detect a runtime toggle and realloc (mirrors volQualityCached).
        const bool giHalf = m_System.GetRestirGiSettings().halfResolution;
        const u32  giW    = giHalf ? halfW : fullW;
        const u32  giH    = giHalf ? halfH : fullH;
        vr.giHalfCached   = giHalf ? 1u : 0u;

        // Half-res DI (RestirSettings::halfResolution): both DI channels (diffuse + specular) trace +
        // denoise at half; svgfDenoised + svgfDiSpecDenoised stay full (the bilateral-upscale outputs).
        const bool diHalf = m_System.GetRestirSettings().halfResolution;
        const u32  diW    = diHalf ? halfW : fullW;
        const u32  diH    = diHalf ? halfH : fullH;
        vr.diHalfCached   = diHalf ? 1u : 0u;

        // Half-res reflections (ReflectionsSettings::halfResolution): reflRadiance trace output + svgfSpec*
        // history allocate at half; svgfSpecDenoised stays full (the bilateral-upscale output).
        const bool reflHalf = m_System.GetReflectionsSettings().halfResolution;
        const u32  reflW    = reflHalf ? halfW : fullW;
        const u32  reflH    = reflHalf ? halfH : fullH;
        vr.reflHalfCached   = reflHalf ? 1u : 0u;

        // Bloom pyramid mips — RGBA16F STORAGE+SAMPLED, mip[0] at half-res then halving each level.
        // STORAGE for the compute prefilter/down/up imageStore + SAMPLED (ctor) for the next stage's
        // filtered taps. The ctor leaves them SHADER_READ_ONLY, so the bloomStrength==0 skip (passes
        // not registered) keeps the composite's stale bloom binding valid — no bootstrap clear needed
        // (every mip is fully written before read each frame; no cross-frame dependency).
        for (u32 i = 0; i < ViewResources::kBloomMipCount; ++i)
        {
            const u32 mipW = std::max(halfW >> i, 1u);
            const u32 mipH = std::max(halfH >> i, 1u);
            vr.bloomMip[i] = std::make_shared<VKTexture>(
                mipW, mipH, TextureFormat::RGBA16F,
                /*arrayLayers*/ 1, /*createFlags*/ 0u, /*mipLevels*/ 1,
                VK_IMAGE_USAGE_STORAGE_BIT);
        }

        // TAA history (Karis14 YCoCg-clip recipe) — viewport-sized RGBA16F HDR. Persistent across
        // frames; ping-pong via frameAbs parity. SAMPLED for the resolve's history read; COLOR
        // attachment for the resolve's write. Frame 0 settles via off-screen UV rejection in
        // the resolve shader (motion vectors land outside [0,1] when prevViewProj is identity).
        vr.taaHistoryA = Texture::Create(fullW, fullH, TextureFormat::RGBA16F);
        vr.taaHistoryB = Texture::Create(fullW, fullH, TextureFormat::RGBA16F);

        // RT sun-shadow mask — viewport-sized R8 storage. Written by rt_sun_shadows.comp on
        // AsyncCompute, sampled by pbr.frag (Set 3 binding 4) when ShadowingMode == RtShadows.
        vr.sunShadowMask = std::make_shared<VKTexture>(
            fullW, fullH, TextureFormat::R8,
            /*arrayLayers*/ 1, /*createFlags*/ 0u, /*mipLevels*/ 1,
            VK_IMAGE_USAGE_STORAGE_BIT);

        // ReSTIR DI demodulated-irradiance image — DI working res (half when halfResolution). STORAGE for
        // the shade pass's imageStore + SAMPLED (ctor) for the denoiser input.
        vr.restirDI = std::make_shared<VKTexture>(
            diW, diH, TextureFormat::RGBA16F,
            /*arrayLayers*/ 1, /*createFlags*/ 0u, /*mipLevels*/ 1,
            VK_IMAGE_USAGE_STORAGE_BIT);

        // ReSTIR-DI demodulated SPECULAR image (#154) — same DI working res; shade's b8 imageStore +
        // SAMPLED (ctor) for the DiSpecular denoiser. Written each frame → no bootstrap clear.
        vr.restirDISpec = std::make_shared<VKTexture>(
            diW, diH, TextureFormat::RGBA16F,
            /*arrayLayers*/ 1, /*createFlags*/ 0u, /*mipLevels*/ 1,
            VK_IMAGE_USAGE_STORAGE_BIT);

        // ReSTIR GI demodulated indirect-diffuse image — GI working res (half when halfResolution).
        // STORAGE for the GI shade pass's imageStore + SAMPLED (ctor) for the denoiser input.
        vr.restirGiDI = std::make_shared<VKTexture>(
            giW, giH, TextureFormat::RGBA16F,
            /*arrayLayers*/ 1, /*createFlags*/ 0u, /*mipLevels*/ 1,
            VK_IMAGE_USAGE_STORAGE_BIT);

        // Path-traced reference mode (rt-renderer C.5). ptAccum = viewport-sized RGBA32F STORAGE — the
        // in-place fp32 progressive running mean, kept GENERAL, only ever touched by the PT megakernel
        // (read-before-write cross-frame → bootstrap-cleared below). ptColor = RGBA16F STORAGE+SAMPLED
        // display copy the post chain samples; fully written each frame → no clear (svgfDenoised pattern).
        vr.ptAccum = std::make_shared<VKTexture>(
            fullW, fullH, TextureFormat::RGBA32F,
            /*arrayLayers*/ 1, /*createFlags*/ 0u, /*mipLevels*/ 1,
            VK_IMAGE_USAGE_STORAGE_BIT);
        vr.ptColor = std::make_shared<VKTexture>(
            fullW, fullH, TextureFormat::RGBA16F,
            /*arrayLayers*/ 1, /*createFlags*/ 0u, /*mipLevels*/ 1,
            VK_IMAGE_USAGE_STORAGE_BIT);

        // RT specular reflections (rt-renderer D.1) — reflection working res (half when halfResolution).
        // STORAGE for the trace's imageStore + SAMPLED (ctor) for pbr.frag's Set 3 b7 read. rgb =
        // demodulated specular radiance, a = hitDist. Fully written each frame (reflection or env fallback)
        // → no bootstrap clear.
        vr.reflRadiance = std::make_shared<VKTexture>(
            reflW, reflH, TextureFormat::RGBA16F,
            /*arrayLayers*/ 1, /*createFlags*/ 0u, /*mipLevels*/ 1,
            VK_IMAGE_USAGE_STORAGE_BIT);

        // SVGF denoiser output — same shape as restirDI. The denoiser reads restirDI and writes the
        // denoised result here; pbr.frag Set 3 b5 samples this. Written before read each frame, so no
        // bootstrap clear needed.
        vr.svgfDenoised = std::make_shared<VKTexture>(
            fullW, fullH, TextureFormat::RGBA16F,
            /*arrayLayers*/ 1, /*createFlags*/ 0u, /*mipLevels*/ 1,
            VK_IMAGE_USAGE_STORAGE_BIT);

        // SVGF temporal history (ping-pong) — RGBA16F storage, kept GENERAL. colorHist = color+variance,
        // moments = (mu1,mu2,histLen), geom = (linearZ, octN.xy). Read-before-write on frame 0 → bootstrap-
        // cleared with the volumetric history below.
        for (u32 i = 0; i < 2; ++i)
        {
            vr.svgfColorHist[i] = std::make_shared<VKTexture>(diW, diH, TextureFormat::RGBA16F, 1, 0u, 1, VK_IMAGE_USAGE_STORAGE_BIT);
            vr.svgfMoments[i]   = std::make_shared<VKTexture>(diW, diH, TextureFormat::RGBA16F, 1, 0u, 1, VK_IMAGE_USAGE_STORAGE_BIT);
            vr.svgfGeom[i]      = std::make_shared<VKTexture>(diW, diH, TextureFormat::RGBA16F, 1, 0u, 1, VK_IMAGE_USAGE_STORAGE_BIT);
        }

        // À-trous ping-pong — same shape as the SVGF history. svgfAtrous[0] is the moments output (à-trous
        // level-0 input); the wavelet levels ping-pong [0]/[1]. Bootstrap-cleared to GENERAL below.
        vr.svgfAtrous[0] = std::make_shared<VKTexture>(diW, diH, TextureFormat::RGBA16F, 1, 0u, 1, VK_IMAGE_USAGE_STORAGE_BIT);
        vr.svgfAtrous[1] = std::make_shared<VKTexture>(diW, diH, TextureFormat::RGBA16F, 1, 0u, 1, VK_IMAGE_USAGE_STORAGE_BIT);
        // Half-res DI diffuse à-trous final (upscale input); svgfDenoised stays full. Written each frame.
        vr.svgfDiHalf = std::make_shared<VKTexture>(diW, diH, TextureFormat::RGBA16F, 1, 0u, 1, VK_IMAGE_USAGE_STORAGE_BIT);

        // GI SVGF — flat parallel set to the DI history above (S4). History + à-trous run at the GI
        // working res (half when halfResolution); svgfGiDenoised stays FULL (the bilateral-upscale
        // output). svgfGiHalf is the half à-trous final the upscale reads; written each frame → no clear.
        vr.svgfGiDenoised = std::make_shared<VKTexture>(fullW, fullH, TextureFormat::RGBA16F, 1, 0u, 1, VK_IMAGE_USAGE_STORAGE_BIT);
        vr.svgfGiHalf     = std::make_shared<VKTexture>(giW, giH, TextureFormat::RGBA16F, 1, 0u, 1, VK_IMAGE_USAGE_STORAGE_BIT);
        for (u32 i = 0; i < 2; ++i)
        {
            vr.svgfGiColorHist[i] = std::make_shared<VKTexture>(giW, giH, TextureFormat::RGBA16F, 1, 0u, 1, VK_IMAGE_USAGE_STORAGE_BIT);
            vr.svgfGiMoments[i]   = std::make_shared<VKTexture>(giW, giH, TextureFormat::RGBA16F, 1, 0u, 1, VK_IMAGE_USAGE_STORAGE_BIT);
            vr.svgfGiGeom[i]      = std::make_shared<VKTexture>(giW, giH, TextureFormat::RGBA16F, 1, 0u, 1, VK_IMAGE_USAGE_STORAGE_BIT);
        }
        vr.svgfGiAtrous[0] = std::make_shared<VKTexture>(giW, giH, TextureFormat::RGBA16F, 1, 0u, 1, VK_IMAGE_USAGE_STORAGE_BIT);
        vr.svgfGiAtrous[1] = std::make_shared<VKTexture>(giW, giH, TextureFormat::RGBA16F, 1, 0u, 1, VK_IMAGE_USAGE_STORAGE_BIT);

        // Specular (RT-reflection) SVGF history (D.1) — flat parallel to the GI SVGF. History + à-trous run
        // at the reflection working res (half when halfResolution); svgfSpecDenoised stays FULL (the
        // bilateral-upscale output). svgfSpecGeom carries hitDist in its .a (vs GI's unused .a) for
        // reflected-depth disocclusion. svgfSpecHalf is the half à-trous final the upscale reads; written
        // each frame → no clear. Cross-frame history below is bootstrap-cleared (at reflW/reflH).
        vr.svgfSpecDenoised = std::make_shared<VKTexture>(fullW, fullH, TextureFormat::RGBA16F, 1, 0u, 1, VK_IMAGE_USAGE_STORAGE_BIT);
        vr.svgfSpecHalf     = std::make_shared<VKTexture>(reflW, reflH, TextureFormat::RGBA16F, 1, 0u, 1, VK_IMAGE_USAGE_STORAGE_BIT);
        for (u32 i = 0; i < 2; ++i)
        {
            vr.svgfSpecColorHist[i] = std::make_shared<VKTexture>(reflW, reflH, TextureFormat::RGBA16F, 1, 0u, 1, VK_IMAGE_USAGE_STORAGE_BIT);
            vr.svgfSpecMoments[i]   = std::make_shared<VKTexture>(reflW, reflH, TextureFormat::RGBA16F, 1, 0u, 1, VK_IMAGE_USAGE_STORAGE_BIT);
            vr.svgfSpecGeom[i]      = std::make_shared<VKTexture>(reflW, reflH, TextureFormat::RGBA16F, 1, 0u, 1, VK_IMAGE_USAGE_STORAGE_BIT);
        }
        vr.svgfSpecAtrous[0] = std::make_shared<VKTexture>(reflW, reflH, TextureFormat::RGBA16F, 1, 0u, 1, VK_IMAGE_USAGE_STORAGE_BIT);
        vr.svgfSpecAtrous[1] = std::make_shared<VKTexture>(reflW, reflH, TextureFormat::RGBA16F, 1, 0u, 1, VK_IMAGE_USAGE_STORAGE_BIT);

        // ReSTIR-DI specular SVGF history (#154) — flat parallel to the reflection-spec SVGF above.
        // Bootstrap-cleared below (cross-frame temporal read).
        vr.svgfDiSpecDenoised = std::make_shared<VKTexture>(fullW, fullH, TextureFormat::RGBA16F, 1, 0u, 1, VK_IMAGE_USAGE_STORAGE_BIT);
        for (u32 i = 0; i < 2; ++i)
        {
            vr.svgfDiSpecColorHist[i] = std::make_shared<VKTexture>(diW, diH, TextureFormat::RGBA16F, 1, 0u, 1, VK_IMAGE_USAGE_STORAGE_BIT);
            vr.svgfDiSpecMoments[i]   = std::make_shared<VKTexture>(diW, diH, TextureFormat::RGBA16F, 1, 0u, 1, VK_IMAGE_USAGE_STORAGE_BIT);
            vr.svgfDiSpecGeom[i]      = std::make_shared<VKTexture>(diW, diH, TextureFormat::RGBA16F, 1, 0u, 1, VK_IMAGE_USAGE_STORAGE_BIT);
        }
        vr.svgfDiSpecAtrous[0] = std::make_shared<VKTexture>(diW, diH, TextureFormat::RGBA16F, 1, 0u, 1, VK_IMAGE_USAGE_STORAGE_BIT);
        vr.svgfDiSpecAtrous[1] = std::make_shared<VKTexture>(diW, diH, TextureFormat::RGBA16F, 1, 0u, 1, VK_IMAGE_USAGE_STORAGE_BIT);
        // Half-res DI specular à-trous final (upscale input); svgfDiSpecDenoised stays full.
        vr.svgfDiSpecHalf = std::make_shared<VKTexture>(diW, diH, TextureFormat::RGBA16F, 1, 0u, 1, VK_IMAGE_USAGE_STORAGE_BIT);

        // ReSTIR reservoir ping-pong pair — Garlic device-local large-tagged, reused across frames.
        // Destroyed via FreeTagAndDestroy on resize (deferred N+2; resize-sized, so recycling would
        // orphan the old size). The tags stay in the reserved high range so the per-frame FreeTag(N-2)
        // sweep never touches them. Two distinct tags so temporal reuse can read last frame's reservoir
        // (prev) while writing this frame's (curr). Re-allocate here — sized w*h.
        for (u32 i = 0; i < 2; ++i)
        {
            if (vr.restirReservoirTag[i] != 0)
            {
                Memory::GPUTaggedPageAllocator::Get().FreeTagAndDestroy(vr.restirReservoirTag[i]);
                vr.restirReservoir[i] = {};
            }
            vr.restirReservoirTag[i] = m_Restir.NextReservoirTag();
            vr.restirReservoir[i] = Memory::GPUTaggedPageAllocator::Get().AllocateLargeTaggedDeviceLocal(
                vr.restirReservoirTag[i], static_cast<u64>(diW) * static_cast<u64>(diH) * 32u, 16);
        }

        // ReSTIR spatial-reuse output — a SINGLE device-local buffer (no ping-pong). The spatial pass
        // reads b2 (temporal output, read-only) and writes here; shade consumes it. Overwritten +
        // consumed each frame, so no cross-frame history — one reserved tag, freed on resize/destroy.
        if (vr.restirSpatialTag != 0)
        {
            Memory::GPUTaggedPageAllocator::Get().FreeTagAndDestroy(vr.restirSpatialTag);
            vr.restirSpatial = {};
        }
        vr.restirSpatialTag = m_Restir.NextReservoirTag();
        vr.restirSpatial = Memory::GPUTaggedPageAllocator::Get().AllocateLargeTaggedDeviceLocal(
            vr.restirSpatialTag, static_cast<u64>(diW) * static_cast<u64>(diH) * 32u, 16);

        // ReSTIR GI reservoir ping-pong — same lifecycle as the DI pair but 64 B/pixel (a GIReservoir
        // is a world-space path vertex). Tags from the GI subsystem's disjoint 0xFFFF8000 range.
        for (u32 i = 0; i < 2; ++i)
        {
            if (vr.restirGiReservoirTag[i] != 0)
            {
                Memory::GPUTaggedPageAllocator::Get().FreeTagAndDestroy(vr.restirGiReservoirTag[i]);
                vr.restirGiReservoir[i] = {};
            }
            vr.restirGiReservoirTag[i] = m_RestirGi.NextReservoirTag();
            vr.restirGiReservoir[i] = Memory::GPUTaggedPageAllocator::Get().AllocateLargeTaggedDeviceLocal(
                vr.restirGiReservoirTag[i], static_cast<u64>(giW) * static_cast<u64>(giH) * 64u, 16);
        }

        // ReSTIR GI spatial-reuse output — single device-local buffer (no ping-pong), 64 B/pixel.
        if (vr.restirGiSpatialTag != 0)
        {
            Memory::GPUTaggedPageAllocator::Get().FreeTagAndDestroy(vr.restirGiSpatialTag);
            vr.restirGiSpatial = {};
        }
        vr.restirGiSpatialTag = m_RestirGi.NextReservoirTag();
        vr.restirGiSpatial = Memory::GPUTaggedPageAllocator::Get().AllocateLargeTaggedDeviceLocal(
            vr.restirGiSpatialTag, static_cast<u64>(giW) * static_cast<u64>(giH) * 64u, 16);

        // PPLL OIT heads — R32_Uint storage, cleared by OITClear each frame (no bootstrap clear:
        // the per-frame Undefined import + transfer clear defines layout and content together).
        vr.oitHeads = std::make_shared<VKTexture>(
            fullW, fullH, TextureFormat::R32_Uint,
            /*arrayLayers*/ 1, /*createFlags*/ 0u, /*mipLevels*/ 1,
            VK_IMAGE_USAGE_STORAGE_BIT);

        // PPLL OIT node pool — `{count, pad[3], OITNode[W*H*budget]}`, 16 B/node, Garlic device-local
        // with a reserved tag (reservoir lifecycle: freed only here on realloc, or on view release).
        if (vr.oitNodesTag != 0)
        {
            Memory::GPUTaggedPageAllocator::Get().FreeTagAndDestroy(vr.oitNodesTag);
            vr.oitNodes = {};
        }
        const u32 oitBudget = m_System.GetTransparencySettings().avgLayersBudget;
        vr.oitLayersCached  = oitBudget;
        vr.oitNodesTag      = m_Transparency.NextNodePoolTag();
        vr.oitNodes = Memory::GPUTaggedPageAllocator::Get().AllocateLargeTaggedDeviceLocal(
            vr.oitNodesTag, 16ull + static_cast<u64>(fullW) * static_cast<u64>(fullH) * oitBudget * 16ull, 16);

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
        // pixel content. The volumetric resolve samples volInScatterHist{A,B} and the SVGF reproject
        // imageLoads its prev history on frame 0; without this clear the first read is NaN-prone garbage
        // (and imageLoad needs GENERAL). One-shot submit per view-resize only.
        VkImage clearTargets[36] = {
            std::static_pointer_cast<VKTexture>(vr.volInScatter)->GetImage(),
            std::static_pointer_cast<VKTexture>(vr.volInScatterHistA)->GetImage(),
            std::static_pointer_cast<VKTexture>(vr.volInScatterHistB)->GetImage(),
            std::static_pointer_cast<VKTexture>(vr.svgfColorHist[0])->GetImage(),
            std::static_pointer_cast<VKTexture>(vr.svgfColorHist[1])->GetImage(),
            std::static_pointer_cast<VKTexture>(vr.svgfMoments[0])->GetImage(),
            std::static_pointer_cast<VKTexture>(vr.svgfMoments[1])->GetImage(),
            std::static_pointer_cast<VKTexture>(vr.svgfGeom[0])->GetImage(),
            std::static_pointer_cast<VKTexture>(vr.svgfGeom[1])->GetImage(),
            std::static_pointer_cast<VKTexture>(vr.svgfAtrous[0])->GetImage(),
            std::static_pointer_cast<VKTexture>(vr.svgfAtrous[1])->GetImage(),
            // GI SVGF history (S4) — bootstrap-cleared so frame 0's prev imageLoad is well-defined.
            std::static_pointer_cast<VKTexture>(vr.svgfGiColorHist[0])->GetImage(),
            std::static_pointer_cast<VKTexture>(vr.svgfGiColorHist[1])->GetImage(),
            std::static_pointer_cast<VKTexture>(vr.svgfGiMoments[0])->GetImage(),
            std::static_pointer_cast<VKTexture>(vr.svgfGiMoments[1])->GetImage(),
            std::static_pointer_cast<VKTexture>(vr.svgfGiGeom[0])->GetImage(),
            std::static_pointer_cast<VKTexture>(vr.svgfGiGeom[1])->GetImage(),
            std::static_pointer_cast<VKTexture>(vr.svgfGiAtrous[0])->GetImage(),
            std::static_pointer_cast<VKTexture>(vr.svgfGiAtrous[1])->GetImage(),
            // PathTrace fp32 accumulator — read-before-write cross-frame, so zero it to GENERAL on resize.
            std::static_pointer_cast<VKTexture>(vr.ptAccum)->GetImage(),
            // Specular SVGF history (D.1) — frame 0's prev imageLoad must be well-defined.
            std::static_pointer_cast<VKTexture>(vr.svgfSpecColorHist[0])->GetImage(),
            std::static_pointer_cast<VKTexture>(vr.svgfSpecColorHist[1])->GetImage(),
            std::static_pointer_cast<VKTexture>(vr.svgfSpecMoments[0])->GetImage(),
            std::static_pointer_cast<VKTexture>(vr.svgfSpecMoments[1])->GetImage(),
            std::static_pointer_cast<VKTexture>(vr.svgfSpecGeom[0])->GetImage(),
            std::static_pointer_cast<VKTexture>(vr.svgfSpecGeom[1])->GetImage(),
            std::static_pointer_cast<VKTexture>(vr.svgfSpecAtrous[0])->GetImage(),
            std::static_pointer_cast<VKTexture>(vr.svgfSpecAtrous[1])->GetImage(),
            // ReSTIR-DI specular SVGF history (#154) — frame 0's prev imageLoad must be well-defined.
            std::static_pointer_cast<VKTexture>(vr.svgfDiSpecColorHist[0])->GetImage(),
            std::static_pointer_cast<VKTexture>(vr.svgfDiSpecColorHist[1])->GetImage(),
            std::static_pointer_cast<VKTexture>(vr.svgfDiSpecMoments[0])->GetImage(),
            std::static_pointer_cast<VKTexture>(vr.svgfDiSpecMoments[1])->GetImage(),
            std::static_pointer_cast<VKTexture>(vr.svgfDiSpecGeom[0])->GetImage(),
            std::static_pointer_cast<VKTexture>(vr.svgfDiSpecGeom[1])->GetImage(),
            std::static_pointer_cast<VKTexture>(vr.svgfDiSpecAtrous[0])->GetImage(),
            std::static_pointer_cast<VKTexture>(vr.svgfDiSpecAtrous[1])->GetImage(),
        };
        constexpr u32 kClearCount = 36;
        VulkanContext::Get().ImmediateSubmit([&](VkCommandBuffer cmd) {
            VkImageMemoryBarrier toDst[kClearCount]{};
            for (u32 i = 0; i < kClearCount; ++i)
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
                                 0, 0, nullptr, 0, nullptr, kClearCount, toDst);

            VkClearColorValue zero{ { 0.0f, 0.0f, 0.0f, 0.0f } };
            VkImageSubresourceRange range{ VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
            for (u32 i = 0; i < kClearCount; ++i)
                vkCmdClearColorImage(cmd, clearTargets[i], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &zero, 1, &range);

            // OIT heads — uint image, separate clear value (OIT_EMPTY = 0xFFFFFFFF). Bootstrapping to
            // GENERAL here makes the per-frame import's claimed initial state (FragmentStorageRead)
            // true on frame 0; the cross-frame WAR ordering relies on that claimed src stage.
            VkImage headsImg = std::static_pointer_cast<VKTexture>(vr.oitHeads)->GetImage();
            VkImageMemoryBarrier headsToDst{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
            headsToDst.oldLayout           = VK_IMAGE_LAYOUT_UNDEFINED;
            headsToDst.newLayout           = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            headsToDst.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            headsToDst.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            headsToDst.image               = headsImg;
            headsToDst.subresourceRange    = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
            headsToDst.srcAccessMask       = 0;
            headsToDst.dstAccessMask       = VK_ACCESS_TRANSFER_WRITE_BIT;
            vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                 0, 0, nullptr, 0, nullptr, 1, &headsToDst);
            VkClearColorValue headsClear{};
            headsClear.uint32[0] = 0xFFFFFFFFu;
            vkCmdClearColorImage(cmd, headsImg, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &headsClear, 1, &range);

            VkImageMemoryBarrier toGen[kClearCount + 1]{};
            for (u32 i = 0; i < kClearCount + 1; ++i)
            {
                toGen[i].sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
                toGen[i].oldLayout           = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
                toGen[i].newLayout           = VK_IMAGE_LAYOUT_GENERAL;
                toGen[i].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                toGen[i].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                toGen[i].image               = (i < kClearCount) ? clearTargets[i] : headsImg;
                toGen[i].subresourceRange    = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
                toGen[i].srcAccessMask       = VK_ACCESS_TRANSFER_WRITE_BIT;
                toGen[i].dstAccessMask       = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
            }
            vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                 VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                                 0, 0, nullptr, 0, nullptr, kClearCount + 1, toGen);
        });
    }

    void RenderPipeline::DestroyViewResources(ViewResources& vr)
    {
        // Pool destruction frees every descriptor set allocated from it.
        for (auto& mip : vr.bloomMip) mip.reset();
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
        vr.sunShadowMask.reset();
        vr.restirDI.reset();
        vr.restirDISpec.reset();
        vr.restirGiDI.reset();
        vr.svgfDenoised.reset();
        for (u32 i = 0; i < 2; ++i)
        {
            vr.svgfColorHist[i].reset();
            vr.svgfMoments[i].reset();
            vr.svgfGeom[i].reset();
        }
        vr.svgfAtrous[0].reset();
        vr.svgfAtrous[1].reset();
        vr.svgfGiDenoised.reset();
        vr.svgfGiHalf.reset();
        vr.svgfDiHalf.reset();
        vr.svgfDiSpecHalf.reset();
        for (u32 i = 0; i < 2; ++i)
        {
            vr.svgfGiColorHist[i].reset();
            vr.svgfGiMoments[i].reset();
            vr.svgfGiGeom[i].reset();
        }
        vr.svgfGiAtrous[0].reset();
        vr.svgfGiAtrous[1].reset();
        vr.ptAccum.reset();
        vr.ptColor.reset();
        vr.reflRadiance.reset();
        vr.svgfSpecDenoised.reset();
        vr.svgfSpecHalf.reset();
        for (u32 i = 0; i < 2; ++i)
        {
            vr.svgfSpecColorHist[i].reset();
            vr.svgfSpecMoments[i].reset();
            vr.svgfSpecGeom[i].reset();
        }
        vr.svgfSpecAtrous[0].reset();
        vr.svgfSpecAtrous[1].reset();
        vr.svgfDiSpecDenoised.reset();
        for (u32 i = 0; i < 2; ++i)
        {
            vr.svgfDiSpecColorHist[i].reset();
            vr.svgfDiSpecMoments[i].reset();
            vr.svgfDiSpecGeom[i].reset();
        }
        vr.svgfDiSpecAtrous[0].reset();
        vr.svgfDiSpecAtrous[1].reset();
        vr.oitHeads.reset();

        // OIT node-pool reserved tag — same deferred-destroy path as the reservoir tags below.
        if (vr.oitNodesTag != 0)
        {
            Memory::GPUTaggedPageAllocator::Get().FreeTagAndDestroy(vr.oitNodesTag);
            vr.oitNodesTag = 0;
            vr.oitNodes = {};
        }

        // Release both reservoir reserved-range tags. The buffers are destroyed (deferred N+2), not
        // recycled — they're resize-sized; the high tags keep them out of the per-frame FreeTag(N-2) sweep.
        for (u32 i = 0; i < 2; ++i)
        {
            if (vr.restirReservoirTag[i] != 0)
            {
                Memory::GPUTaggedPageAllocator::Get().FreeTagAndDestroy(vr.restirReservoirTag[i]);
                vr.restirReservoirTag[i] = 0;
                vr.restirReservoir[i] = {};
            }
        }
        if (vr.restirSpatialTag != 0)
        {
            Memory::GPUTaggedPageAllocator::Get().FreeTagAndDestroy(vr.restirSpatialTag);
            vr.restirSpatialTag = 0;
            vr.restirSpatial = {};
        }

        // ReSTIR GI reserved-range tags — same deferred-destroy path as the DI tags above.
        for (u32 i = 0; i < 2; ++i)
        {
            if (vr.restirGiReservoirTag[i] != 0)
            {
                Memory::GPUTaggedPageAllocator::Get().FreeTagAndDestroy(vr.restirGiReservoirTag[i]);
                vr.restirGiReservoirTag[i] = 0;
                vr.restirGiReservoir[i] = {};
            }
        }
        if (vr.restirGiSpatialTag != 0)
        {
            Memory::GPUTaggedPageAllocator::Get().FreeTagAndDestroy(vr.restirGiSpatialTag);
            vr.restirGiSpatialTag = 0;
            vr.restirGiSpatial = {};
        }

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
