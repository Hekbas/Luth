#pragma once

#include "luth/core/types/LuthMath.h"
#include "luth/renderer/CameraParams.h"
#include "luth/renderer/lighting/LightTypes.h"

#include <vulkan/vulkan.h>

namespace Luth
{
    class RenderPipeline;
    class LightingSubsystem;
    struct ViewResources;

    // Inputs the per-view Set 0 writer needs from other subsystems / RP scratch.
    // Filled by RenderPipeline::AllocateViewResources before calling WriteView.
    struct GlobalViewWriteContext
    {
        // IBL bindings 1-3 (skipped if !haveIBL; InitIBLResources hasn't run yet).
        bool        haveIBL = false;
        VkImageView irradianceView   = VK_NULL_HANDLE;
        VkImageView prefilteredView  = VK_NULL_HANDLE;
        VkImageView brdfView         = VK_NULL_HANDLE;
        VkSampler   iblSampler       = VK_NULL_HANDLE;

        // GTAO binding 4 (final-AO sampler). Required.
        VkImageView gtaoFinalView = VK_NULL_HANDLE;
        VkSampler   gtaoSampler   = VK_NULL_HANDLE;
    };

    // Owns Set 0 (Global UBO + IBL samplers + GTAO final + GTAO UBO) layout. Per-frame UBO is rebound to a
    // fresh GPUTaggedPageAllocator region in UpdateUBO; the Grid set's binding 0 shares the same region
    // (atomic 2-set write; see arch/rendering-pipeline.md). The IBL textures and GTAO sampler/final-view
    // are owned by other subsystems and passed in via GlobalViewWriteContext at AllocateViewResources time.
    class GlobalSubsystem
    {
    public:
        void Init(RenderPipeline& pipeline);
        void Shutdown();

        // Caches cascades + shadow params; computes viewProjection; rebinds Set 0 binding 0 + Grid set
        // binding 0 to a fresh per-frame UBO region.
        void UpdateUBO(const CameraParams& camera, const CascadeData& cascades,
                       const DirectionalLightShadowParams& shadowParams);

        // Writes Set 0 bindings 1-4 (IBL irradiance/prefiltered/BRDF + GTAO sampler).
        // Bindings 0 + 5 are rewritten per render-stage in UpdateUBO / GTAO.UpdateUBO.
        void WriteView(ViewResources& vr, const GlobalViewWriteContext& ctx);

        VkDescriptorSetLayout                  GetSetLayout()       const { return m_GlobalSetLayout; }
        const Mat4&                            GetCachedViewProj()  const { return m_CachedViewProj; }
        const CascadeData&                     GetCascades()        const { return m_FrameCascades; }
        const DirectionalLightShadowParams&    GetShadowParams()    const { return m_FrameShadowParams; }

        // Copies the most-recent UpdateUBO call's GPU-true GlobalUniforms bytes into `out`. Used by the frame
        // debugger to snapshot the captured view's camera UBO at FinalizeCapture time so replay can re-bind it.
        void GetLastUboBytes(std::vector<u8>& out) const { out = m_LastUboBytes; }

    private:
        RenderPipeline*       m_Pipeline         = nullptr;
        VkDescriptorSetLayout m_GlobalSetLayout  = VK_NULL_HANDLE;

        // Per-frame scratch: set in UpdateUBO, read by Execute + frame-debugger CaptureSnapshot.
        CascadeData                        m_FrameCascades{};
        DirectionalLightShadowParams       m_FrameShadowParams{};
        Mat4                               m_CachedViewProj{ 1.0f };
        std::vector<u8>                    m_LastUboBytes;   // serialized GlobalUniforms, last UpdateUBO
    };
}
