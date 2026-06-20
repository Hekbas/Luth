#pragma once

#include "luth/core/types/LuthTypes.h"
#include "luth/renderer/rendergraph/RenderGraph.h"
#include "luth/renderer/backend/vulkan/VulkanComputePipeline.h"

#include <memory>
#include <string>
#include <vector>

namespace Luth
{
    class RenderPipeline;
    class FrameTargets;
    struct ViewResources;

    // RT specular reflections (rt-renderer D.1). A rayQuery-in-compute pass that casts one GGX-VNDF
    // reflection ray per opaque pixel from the slim G-buffer (oct normal + roughness + depth), shades the
    // hit via the bindless geometry table + NEE, and writes a DEMODULATED specular-radiance image (rgb)
    // + hitDist (a). A dedicated specular denoiser cleans it; pbr.frag re-modulates it into the split-sum
    // specular slot above a roughness cutoff. Supersedes SSR. Opaque-only (gl_RayFlagsOpaqueEXT).
    //
    // Mirrors PathTraceSubsystem's compute-RT shape (5-set bind, inline AS barrier dst=COMPUTE, geom-table
    // BDA push constant) and RtRestirGiSubsystem's slim-G-buffer input pattern. S0 is the seam: one
    // pipeline + pass-local Set 2 (b0 output, b1-b3 slim inputs) + a stub test-pattern shader, kept alive
    // via SetHasSideEffect (no consumer until S4 composites it). see arch/rendering-pipeline.md
    class ReflectionsSubsystem
    {
    public:
        void Init(RenderPipeline& pipeline);
        void Shutdown();

        bool OnShaderReloaded(const std::string& name, const std::vector<u32>& spv);

        // Stable per-view Set 2 writes: b0 reflection output (GENERAL storage), b1 depth, b2 slim normal,
        // b3 slim roughness (SHADER_READ_ONLY samplers). Written once at view alloc / resize.
        void WriteView(ViewResources& vr, FrameTargets& targets);

        // Reflection trace dispatch → writes the demodulated reflection image. Returns its handle
        // (invalid when disabled / no view). AsyncCompute, after the TLAS build. Reads the slim G-buffer
        // (handles threaded for RG barrier ordering).
        RG::ResourceHandle AddPasses(RG::RenderGraph& rg,
                                     RG::ResourceHandle sceneDepth,
                                     RG::ResourceHandle slimNormal,
                                     RG::ResourceHandle slimRoughness);

        VkDescriptorSetLayout GetSetLayout() const { return m_SetLayout; }

        // Half-res reflections bilateral upscale (shared bilateral_upscale.comp): resolves the half-res
        // svgfSpecHalf into the full-res svgfSpecDenoised, depth/normal-guided. Only wired when
        // ReflectionsSettings::halfResolution. Mirrors RtRestirGiSubsystem's upscale.
        VkDescriptorSetLayout GetUpscaleLayout() const { return m_UpscaleSetLayout; }
        void WriteUpscaleView(ViewResources& vr, FrameTargets& targets);
        RG::ResourceHandle AddUpscalePass(RG::RenderGraph& rg, RG::ResourceHandle reflHalf,
                                          RG::ResourceHandle sceneDepth, RG::ResourceHandle slimNormal);

        // ReflectionsSettings::enabled is the gate. Out-of-line: needs the RenderingSystem definition,
        // which can't be pulled into this header (RenderPipeline include cycle).
        bool IsEnabled() const;

    private:
        RenderPipeline* m_Pipeline = nullptr;

        std::unique_ptr<VKComputePipeline> m_ReflPipeline;
        VkDescriptorSetLayout              m_SetLayout = VK_NULL_HANDLE;  // Set 2 (pass-local)
        VkSampler                          m_Sampler   = VK_NULL_HANDLE;  // linear clamp — slim G-buffer reads
        std::vector<u32>                   m_Spv;

        std::unique_ptr<VKComputePipeline> m_UpscalePipeline;             // half-res bilateral upscale
        VkDescriptorSetLayout              m_UpscaleSetLayout = VK_NULL_HANDLE;  // Set 1 for the upscale pass
        std::vector<u32>                   m_UpscaleSpv;
    };
}
