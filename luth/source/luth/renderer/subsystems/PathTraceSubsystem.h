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
    struct ViewResources;

    // Path-traced reference mode. A rayQuery-in-compute MEGAKERNEL that reuses the RT TLAS + bindless
    // material infra to brute-force a physically-correct image, progressively accumulated across frames
    // (reset on camera/scene change). It is the ground-truth oracle that validates ReSTIR DI/GI
    // convergence: the full image, not an overlay, so it bypasses raster + ReSTIR. The top-level
    // RenderMode::PathTrace toggle gates it (GlobalSubsystem reads it for pathTraceParams +
    // RenderPipeline::BuildGraph for the raster bypass).
    //
    // The seam is one compute pipeline + the pass-local Set 2 (b0 fp32 accumulator, b1 fp16 display
    // image) + the AddPasses dispatch that writes the display image the post chain samples.
    // see arch/rendering-pipeline.md
    class PathTraceSubsystem
    {
    public:
        void Init(RenderPipeline& pipeline);
        void Shutdown();

        bool OnShaderReloaded(const std::string& name, const std::vector<u32>& spv);

        // Stable per-view Set 2 writes: b0 fp32 accumulator, b1 fp16 display color. The images are
        // persistent (never cycled), so this is written once at view alloc / resize.
        void WriteView(ViewResources& vr);

        // Megakernel dispatch -> writes ptColor (the resolved HDR the post chain samples). Returns that
        // image's handle (invalid when disabled / no TLAS). AsyncCompute, after the TLAS build.
        RG::ResourceHandle AddPasses(RG::RenderGraph& rg);

        VkDescriptorSetLayout GetSetLayout() const { return m_SetLayout; }

        // RenderMode::PathTrace is the gate. Out-of-line: needs the RenderingSystem definition, which
        // can't be pulled into this header (RenderPipeline include cycle).
        bool IsEnabled() const;

        // Editor "Reset" button -> forces the accumulation to restart next frame across all views (the
        // salt folds into the per-view reset hash, so a multi-view setup resets coherently).
        void RequestReset() { ++m_ResetSalt; }

        // Accumulated sample count of the last view dispatched this frame: the editor's convergence readout.
        u32 GetLastSampleCount() const { return m_LastSampleCount; }

    private:
        // FNV-1a over the radiance-affecting state (camera VP + scene instances + lights + settings +
        // manual salt). Compared against ViewResources::ptResetHash each frame; a mismatch resets.
        u64 ComputeResetHash() const;

        RenderPipeline* m_Pipeline = nullptr;

        std::unique_ptr<VKComputePipeline> m_PtPipeline;
        VkDescriptorSetLayout              m_SetLayout = VK_NULL_HANDLE;   // Set 2 (pass-local)
        std::vector<u32>                   m_Spv;
        u32                                m_ResetSalt = 0;               // bumped by RequestReset()
        u32                                m_LastSampleCount = 0;         // editor convergence readout
    };
}
