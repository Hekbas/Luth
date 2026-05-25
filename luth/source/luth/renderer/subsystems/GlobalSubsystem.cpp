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

        // Set 0 layout: 0 = GlobalUBO, 1-3 = IBL samplers, 4 = GTAO sampler, 5 = GTAO UBO.
        VkDescriptorSetLayoutBinding bindings[6] = {};

        bindings[0].binding = 0;
        bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        bindings[0].descriptorCount = 1;
        // COMPUTE added so VolumetricSubsystem's inject pass can sample camera/CSM uniforms.
        bindings[0].stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT
                               | VK_SHADER_STAGE_COMPUTE_BIT;

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

        // invariant: cycled per-frame slots still need UAB — write-vs-still-pending
        // races slip past the slot rotation in practice (validation layer 03047).
        VkDescriptorBindingFlags bindingFlags[6] = {};
        for (auto& f : bindingFlags) f = VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT;
        VkDescriptorSetLayoutBindingFlagsCreateInfo bindingFlagsCI{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO };
        bindingFlagsCI.bindingCount  = 6;
        bindingFlagsCI.pBindingFlags = bindingFlags;

        VkDescriptorSetLayoutCreateInfo layoutInfo{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
        layoutInfo.pNext        = &bindingFlagsCI;
        layoutInfo.flags        = VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT;
        layoutInfo.bindingCount = 6;
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
        Vec2 thisFrameJitter{ 0.0f, 0.0f };
        if (vr && pps.taaEnabled && vr->width > 0 && vr->height > 0)
        {
            const u64 frameAbs = Renderer::GetFrameData()->GetRenderFrameIndex();
            thisFrameJitter    = TAA::SampleHalton(frameAbs);
            ubo.projection     = TAA::ApplyJitter(ubo.projection, thisFrameJitter, vr->width, vr->height);
        }
        ubo.viewProjection = ubo.projection * ubo.view;

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
            // Cache prev/curr jitter so the resolve shader can dejitter sample positions.
            vr->prevJitter    = vr->currentJitter;
            vr->currentJitter = thisFrameJitter;
        } else {
            ubo.prevViewProjection = ubo.viewProjection;  // no view yet → zero motion
            ubo.viewportSize       = Vec2(0.0f);
            ubo.prevViewParams     = Vec4(camera.nearZ, camera.farZ, 0.0f, 0.0f);
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

        VkWriteDescriptorSet writes[2] = {};
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
