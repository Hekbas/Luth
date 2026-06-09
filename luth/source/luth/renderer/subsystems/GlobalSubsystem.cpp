#include "luthpch.h"
#include "luth/renderer/subsystems/GlobalSubsystem.h"
#include "luth/renderer/RenderPipeline.h"
#include "luth/renderer/Renderer.h"
#include "luth/renderer/TaaJitter.h"
#include "luth/renderer/backend/vulkan/VulkanContext.h"
#include "luth/scene/systems/RenderingSystem.h"
#include "luth/core/FrameData.h"
#include "luth/core/time/Time.h"
#include "luth/jobs/JobSystem.h"
#include "luth/memory/GPUTaggedPageAllocator.h"

namespace Luth
{
    void GlobalSubsystem::Init(RenderPipeline& pipeline)
    {
        m_Pipeline = &pipeline;
        VkDevice device = VulkanContext::Get().GetDevice();

        // Set 0 layout: 0 = GlobalUBO, 1-3 = IBL samplers, 4 = GTAO sampler, 5 = GTAO UBO, 6 = TLAS.
        VkDescriptorSetLayoutBinding bindings[7] = {};

        bindings[0].binding = 0;
        bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        bindings[0].descriptorCount = 1;
        // COMPUTE added so VolumetricSubsystem's inject pass can sample camera/CSM uniforms.
        // RAYGEN added so rt_sun_shadows.rgen can read ubo.viewProjection / ubo.rtShadowParams.
        bindings[0].stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT
                               | VK_SHADER_STAGE_COMPUTE_BIT | VK_SHADER_STAGE_RAYGEN_BIT_KHR;

        for (u32 i = 1; i <= 4; ++i)
        {
            bindings[i].binding = i;
            bindings[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            bindings[i].descriptorCount = 1;
            // COMPUTE added so VolumetricSubsystem's inject pass can sample IBL irradiance (b1)
            // for the 2nd-order multi-scatter ambient term.
            bindings[i].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_COMPUTE_BIT;
        }

        bindings[5].binding = 5;
        bindings[5].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        bindings[5].descriptorCount = 1;
        bindings[5].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

        // TLAS binding. Stage flags span ray-query (frag/compute) + future RT-pipeline consumers.
        // PARTIALLY_BOUND keeps boot / scene-empty frames legal — shaders that don't statically
        // access binding 6 (B.2 ships none) don't require the descriptor to be populated.
        bindings[6].binding         = 6;
        bindings[6].descriptorType  = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
        bindings[6].descriptorCount = 1;
        bindings[6].stageFlags      = VK_SHADER_STAGE_FRAGMENT_BIT
                                    | VK_SHADER_STAGE_COMPUTE_BIT
                                    | VK_SHADER_STAGE_RAYGEN_BIT_KHR
                                    | VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR
                                    | VK_SHADER_STAGE_MISS_BIT_KHR;

        // invariant: cycled per-frame slots still need UAB — write-vs-still-pending
        // races slip past the slot rotation in practice (validation layer 03047).
        VkDescriptorBindingFlags bindingFlags[7] = {};
        for (u32 i = 0; i < 6; ++i)
            bindingFlags[i] = VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT;
        bindingFlags[6] = VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT
                        | VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT;
        VkDescriptorSetLayoutBindingFlagsCreateInfo bindingFlagsCI{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO };
        bindingFlagsCI.bindingCount  = 7;
        bindingFlagsCI.pBindingFlags = bindingFlags;

        VkDescriptorSetLayoutCreateInfo layoutInfo{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
        layoutInfo.pNext        = &bindingFlagsCI;
        layoutInfo.flags        = VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT;
        layoutInfo.bindingCount = 7;
        layoutInfo.pBindings    = bindings;
        vkCreateDescriptorSetLayout(device, &layoutInfo, nullptr, &m_GlobalSetLayout);
    }

    void GlobalSubsystem::Shutdown()
    {
        if (m_GlobalSetLayout)
        {
            vkDestroyDescriptorSetLayout(VulkanContext::Get().GetDevice(), m_GlobalSetLayout, nullptr);
            m_GlobalSetLayout = VK_NULL_HANDLE;
        }
    }

    // Allocates a per-frame UBO region from GPUTaggedPageAllocator and rebinds Set 0
    // binding 0 + Grid set binding 0 to it. invariant: the Grid set's binding 0
    // shares this exact region AND the same per-frame slot — the two writes MUST
    // stay in one batched call so we don't double-allocate per frame, and both must
    // use the same `slot` so the next frame's allocator doesn't overwrite a region
    // the previous frame's Grid binding still references (see arch/rendering-pipeline.md).
    void GlobalSubsystem::UpdateUBO(const CameraParams& camera, const CascadeData& cascades,
                                    const DirectionalLightShadowParams& shadowParams)
    {
        m_FrameCascades     = cascades;
        m_FrameShadowParams = shadowParams;

        GlobalUniforms ubo{};
        ubo.view = camera.view;
        ubo.projection = camera.projection;
        ubo.projection[1][1] *= -1.0f;  // Vulkan Y-flip (shader only, not ImGuizmo)

        // Per-view prev-VP + viewport size — stored on ViewResources, NOT on GlobalSubsystem. A single
        // global cross-contaminates between Scene + Game panels (huge motion vectors for static geometry).
        // Frame 0: prevViewProj is Identity → motion nonsense for one frame, settles by frame 1.
        ViewResources* vr = m_Pipeline->GetCurrentViewResources();
        const PostProcessSettings& pps = m_Pipeline->GetSystem().GetPostProcessSettings();

        // TAA jitter — Halton(2,3) sub-pixel offset on the projection matrix BEFORE viewProjection
        // compute. All rendered passes (DepthPrepass, SlimGBuffer, Geometry, Shadow, Skybox) use the
        // jittered projection; motion vectors naturally absorb the jitter delta (standard Karis recipe).
        // Disabled when TAA is off so users don't see pure shimmer with no resolve pass to integrate it.
        // Also disabled in PathTrace mode: the reference accumulates over a STATIC view-projection (the
        // megakernel does its own per-sample jitter), so a moving Halton jitter would restart the
        // accumulation every frame (it feeds the PT reset hash via m_CachedViewProj).
        const bool ptMode = m_Pipeline->GetSystem().GetRenderMode() == RenderMode::PathTrace;
        Vec2 thisFrameJitter{ 0.0f, 0.0f };
        if (vr && pps.taaEnabled && !ptMode && vr->width > 0 && vr->height > 0)
        {
            const u64 frameAbs = Renderer::GetFrameData()->GetRenderFrameIndex();
            thisFrameJitter    = TAA::SampleHalton(frameAbs);
            ubo.projection     = TAA::ApplyJitter(ubo.projection, thisFrameJitter, vr->width, vr->height);
        }
        ubo.viewProjection    = ubo.projection * ubo.view;
        ubo.invViewProjection = Math::Inverse(ubo.viewProjection);

        if (vr) {
            ubo.prevViewProjection = vr->prevViewProj;
            vr->prevViewProj       = ubo.viewProjection;
            ubo.viewportSize       = Vec2(static_cast<float>(vr->width), static_cast<float>(vr->height));
            // Cross-frame near/far cache — read for the resolve pass's reprojection. Bootstrap to
            // current frame's values if uninitialized so frame 0's slice reconstruction is sane.
            const f32 pNearZ = (vr->prevNearZ != 0.0f) ? vr->prevNearZ : camera.nearZ;
            const f32 pFarZ  = (vr->prevFarZ  != 0.0f) ? vr->prevFarZ  : camera.farZ;
            ubo.prevViewParams = Vec4(pNearZ, pFarZ, 0.0f, 0.0f);
            vr->prevNearZ      = camera.nearZ;
            vr->prevFarZ       = camera.farZ;
            // Cache prev/curr jitter. ubo.prevJitter feeds slim_gbuffer.frag's source-side de-jitter
            // (consumed in the next commit); resolve's push-constant jitterDelta still rides
            // ViewResources::prevJitter directly until that move lands.
            vr->prevJitter    = vr->currentJitter;
            vr->currentJitter = thisFrameJitter;
            ubo.prevJitter    = Vec4(vr->prevJitter, 0.0f, 0.0f);
        } else {
            ubo.prevViewProjection = ubo.viewProjection;  // no view yet → zero motion
            ubo.viewportSize       = Vec2(0.0f);
            ubo.prevViewParams     = Vec4(camera.nearZ, camera.farZ, 0.0f, 0.0f);
            ubo.prevJitter         = Vec4(0.0f);
        }
        ubo.nearZ = camera.nearZ;
        ubo.farZ  = camera.farZ;
        ubo.cameraPos = camera.position;
        ubo.time = Time::GetTime();
        for (u32 i = 0; i < k_ShadowCascadeCount; ++i)
            ubo.lightSpaceMatrix[i] = cascades.lightSpaceMatrix[i];
        ubo.cascadeSplitsViewZ = cascades.splitsViewZ;
        // Negative bias (sentinel) disables shadows entirely in the PBR shader.
        ubo.shadowBias       = shadowParams.castShadows ? shadowParams.shadowBias : Vec4(-1.0f);
        ubo.shadowNormalBias = shadowParams.shadowNormalBias;
        ubo.cascadeTexelSize = cascades.texelSize;
        ubo.iblIntensity    = camera.iblIntensity;
        ubo.skyboxIntensity = camera.skyboxIntensity;
        ubo.debugVisualizeCascades = shadowParams.debugVisualizeCascades ? 1.0f : 0.0f;
        ubo.cascadeBlendWidth      = shadowParams.cascadeBlendWidth;

        // Volumetric fog params — distance fog + height fog + multi-scatter + temporal/sun-absorption/
        // sky tunables. heightFogParams.w carries multiScatterIntensity for std140 packing.
        const VolumetricSettings& vs = m_Pipeline->GetSystem().GetVolumetricSettings();
        ubo.distanceFogColorDensity = Vec4(vs.distanceFogColor, vs.distanceFogDensity);
        ubo.distanceFogParams       = Vec4(vs.distanceFogStart, vs.distanceFogMaxOpacity,
                                           vs.distanceFogEnabled ? 1.0f : 0.0f, 0.0f);
        ubo.heightFogColorDensity   = Vec4(vs.heightFogColor, vs.heightFogDensity);
        ubo.heightFogParams         = Vec4(vs.heightFogRefHeight, vs.heightFogFalloff,
                                           vs.heightFogEnabled ? 1.0f : 0.0f, vs.multiScatterIntensity);
        ubo.volTemporalParams       = Vec4(vs.anisotropy,
                                           vs.temporalAlpha,
                                           static_cast<f32>(vs.sunFogAbsorptionSteps),
                                           vs.skyFogStrength);
        ubo.volNoiseParams          = Vec4(vs.noiseScale, vs.noiseStrength, 0.0f, 0.0f);
        ubo.volNoiseWind            = Vec4(vs.noiseWind, 0.0f);
        ubo.volScatterParams        = Vec4(vs.scatteringIntensity,
                                           vs.blueNoiseDither ? 1.0f : 0.0f,
                                           0.0f, 0.0f);

        // Image-quality toggles. Tail of GlobalUniforms — pbr.frag's common/globals.glsl mirrors
        // the std140 layout exactly so offsets stay in lockstep.
        ubo.specAaParams = Vec4(pps.specularAaEnabled ? 1.0f : 0.0f, pps.specularAaSigma, 0.0f, 0.0f);
        ubo.taaParams    = Vec4(pps.taaEnabled ? 1.0f : 0.0f, pps.taaTemporalAlpha,
                                thisFrameJitter.x, thisFrameJitter.y);
        // Sun-shadow path selector + RT world-space epsilons. pbr.frag::ComputeShadow dispatches
        // on .x; raygen reads .y/.z. Disabled-shadows sentinel (shadowBias.x < 0) takes precedence
        // over the mode pick in pbr.frag.
        ubo.rtShadowParams = Vec4(static_cast<f32>(shadowParams.mode),
                                  shadowParams.rtOriginEpsilon,
                                  shadowParams.rtNormalEpsilon,
                                  0.0f);

        // ReSTIR DI consumption flag — set only when the subsystem is enabled AND this view's DI
        // image exists AND a TLAS is available (the conditions under which AddPasses actually writes
        // the DI). Otherwise pbr.frag must run its own point-light loop, so leave x = 0.
        const bool restirActive = m_Pipeline->GetRestir().IsEnabled()
                               && vr && vr->restirDI
                               && m_Pipeline->GetRt().GetTlas() != VK_NULL_HANDLE;
        // .y mirrors .x for the GI path — pbr.frag adds the demodulated indirect-diffuse image when set.
        const bool restirGiActive = m_Pipeline->GetRestirGi().IsEnabled()
                                 && vr && vr->restirGiDI
                                 && m_Pipeline->GetRt().GetTlas() != VK_NULL_HANDLE;
        ubo.restirParams = Vec4(restirActive ? 1.0f : 0.0f, restirGiActive ? 1.0f : 0.0f, 0.0f, 0.0f);

        // Path-traced reference mode (informational — PT bypasses pbr.frag; the megakernel reads its own
        // push constants). x gates nothing in pbr.frag; carried for debug viz + frame-debugger UBO dumps.
        const bool ptActive = m_Pipeline->GetSystem().GetRenderMode() == RenderMode::PathTrace;
        const PathTraceSettings& ptS = m_Pipeline->GetSystem().GetPathTraceSettings();
        const u32 ptSamples = (ptActive && vr) ? vr->ptSampleCount : 0u;
        ubo.pathTraceParams = Vec4(ptActive ? 1.0f : 0.0f, static_cast<f32>(ptS.samplesPerFrame),
                                   static_cast<f32>(ptS.maxBounces), static_cast<f32>(ptSamples));

        // RT reflections (D.1) — gate the pbr.frag composite on enabled AND a valid TLAS (the reflection
        // pass + denoiser no-op before the first build, leaving svgfSpecDenoised stale).
        const ReflectionsSettings& reflS = m_Pipeline->GetSystem().GetReflectionsSettings();
        const bool reflActive = reflS.enabled && m_Pipeline->GetRt().GetTlas() != VK_NULL_HANDLE;
        ubo.reflParams = Vec4(reflActive ? 1.0f : 0.0f, reflS.roughnessFadeStart, reflS.roughnessFadeEnd, 0.0f);

        // m_CachedViewProj is read this frame by cull-compute (frustum) and the frame debugger.
        // Per-view; gets overwritten on each view's UpdateUBO and consumed by the same view's Execute.
        m_CachedViewProj = ubo.viewProjection;

        // Cache GPU-true bytes for the frame debugger's per-view UBO snapshot.
        // FinalizeCapture pulls these out via GetLastUboBytes for replay.
        m_LastUboBytes.resize(sizeof(GlobalUniforms));
        std::memcpy(m_LastUboBytes.data(), &ubo, sizeof(GlobalUniforms));

        if (!vr || vr->globalDescriptorSet[0] == VK_NULL_HANDLE) return;

        auto* jobCtx = JobSystem::GetCurrentJobContext();
        if (!jobCtx) return;
        const u32 frameAbs = static_cast<u32>(Renderer::GetFrameData()->GetRenderFrameIndex());
        const u32 slot     = frameAbs % MAX_FRAMES_IN_FLIGHT;
        jobCtx->GpuCache.CurrentTag = frameAbs;

        auto& heap   = Memory::GPUTaggedPageAllocator::Get();
        const u64 al = VulkanContext::Get().GetMinUniformBufferAlignment();
        Memory::GPUSubRegion region = heap.Allocate(jobCtx->GpuCache, sizeof(GlobalUniforms), al);
        if (!region.buffer) return;

        memcpy(region.mappedPtr, &ubo, sizeof(GlobalUniforms));
        heap.FlushRegion(region);

        VkDescriptorBufferInfo bi{};
        bi.buffer = region.buffer;
        bi.offset = region.offset;
        bi.range  = region.size;

        // TLAS write rides the same per-frame slot rotation as binding 0. Reads the handle the
        // current frame's TlasBuildPass published into RtSubsystem; on frame 0 the handle is null
        // (TlasBuildPass hasn't run yet) — legal under PARTIALLY_BOUND + UAB when no shader
        // statically accesses binding 6, which is the B.2 case (B.3 brings the first reader).
        VkAccelerationStructureKHR tlas = m_Pipeline->GetRt().GetTlas();
        VkWriteDescriptorSetAccelerationStructureKHR asWrite{
            VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR };
        asWrite.accelerationStructureCount = 1;
        asWrite.pAccelerationStructures    = &tlas;

        VkWriteDescriptorSet writes[3] = {};
        writes[0] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
        writes[0].dstSet          = vr->globalDescriptorSet[slot];
        writes[0].dstBinding      = 0;
        writes[0].descriptorType  = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        writes[0].descriptorCount = 1;
        writes[0].pBufferInfo     = &bi;

        u32 n = 1;
        if (vr->gridDescSet[0] != VK_NULL_HANDLE)
        {
            writes[1] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
            writes[1].dstSet          = vr->gridDescSet[slot];
            writes[1].dstBinding      = 0;
            writes[1].descriptorType  = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
            writes[1].descriptorCount = 1;
            writes[1].pBufferInfo     = &bi;
            ++n;
        }

        // TLAS write skipped when handle is null — descriptor stays in its prior populated state
        // (or unbound first frame) per PARTIALLY_BOUND semantics; saves an extra descriptor write.
        if (tlas != VK_NULL_HANDLE)
        {
            writes[n] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
            writes[n].pNext           = &asWrite;
            writes[n].dstSet          = vr->globalDescriptorSet[slot];
            writes[n].dstBinding      = 6;
            writes[n].descriptorType  = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
            writes[n].descriptorCount = 1;
            ++n;
        }

        vkUpdateDescriptorSets(VulkanContext::Get().GetDevice(), n, writes, 0, nullptr);
    }

    void GlobalSubsystem::WriteView(ViewResources& vr, const GlobalViewWriteContext& ctx)
    {
        if (vr.globalDescriptorSet[0] == VK_NULL_HANDLE) return;

        VkDevice device = VulkanContext::Get().GetDevice();

        VkDescriptorImageInfo irrInfo{}, pfInfo{}, lutInfo{};
        if (ctx.haveIBL)
        {
            irrInfo.sampler     = ctx.iblSampler;
            irrInfo.imageView   = ctx.irradianceView;
            irrInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

            pfInfo.sampler      = ctx.iblSampler;
            pfInfo.imageView    = ctx.prefilteredView;
            pfInfo.imageLayout  = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

            lutInfo.sampler     = ctx.iblSampler;
            lutInfo.imageView   = ctx.brdfView;
            lutInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        }

        VkDescriptorImageInfo gtaoFinalInfo{};
        gtaoFinalInfo.sampler     = ctx.gtaoSampler;
        gtaoFinalInfo.imageView   = ctx.gtaoFinalView;
        gtaoFinalInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

        // Stable bindings — propagate to every cycled slot. Allocate-time write;
        // resize path (RenderPipeline::EnsureViewResources) gates on WaitForGPU
        // before re-entering WriteView so no in-flight cmd buffer is bound.
        VkWriteDescriptorSet writes[4 * MAX_FRAMES_IN_FLIGHT] = {};
        u32 n = 0;
        for (u32 s = 0; s < MAX_FRAMES_IN_FLIGHT; ++s)
        {
            if (ctx.haveIBL)
            {
                writes[n] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
                writes[n].dstSet          = vr.globalDescriptorSet[s];
                writes[n].dstBinding      = 1;
                writes[n].descriptorCount = 1;
                writes[n].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
                writes[n].pImageInfo      = &irrInfo;
                ++n;

                writes[n] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
                writes[n].dstSet          = vr.globalDescriptorSet[s];
                writes[n].dstBinding      = 2;
                writes[n].descriptorCount = 1;
                writes[n].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
                writes[n].pImageInfo      = &pfInfo;
                ++n;

                writes[n] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
                writes[n].dstSet          = vr.globalDescriptorSet[s];
                writes[n].dstBinding      = 3;
                writes[n].descriptorCount = 1;
                writes[n].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
                writes[n].pImageInfo      = &lutInfo;
                ++n;
            }

            writes[n] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
            writes[n].dstSet          = vr.globalDescriptorSet[s];
            writes[n].dstBinding      = 4;
            writes[n].descriptorCount = 1;
            writes[n].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            writes[n].pImageInfo      = &gtaoFinalInfo;
            ++n;
        }

        if (n > 0) vkUpdateDescriptorSets(device, n, writes, 0, nullptr);
    }
}
