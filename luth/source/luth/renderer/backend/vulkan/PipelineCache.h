#pragma once

#include "luth/core/types/LuthTypes.h"
#include <vulkan/vulkan.h>

namespace Luth
{
    // VkPipelineCache wrapper plus the project-side persistence. The cache is reused across
    // VKPipeline construction so material and render-mode combos compile faster on second open;
    // LoadFromProject reads <project>/Library/PipelineCache.bin into the live cache and
    // SaveToProject writes it back.
    class PipelineCache
    {
    public:
        // Lifetime tied to the Vulkan device. Init creates an empty cache;
        // project data is merged in / written out via LoadFromProject / SaveToProject.
        static void Init();
        static void Shutdown();

        // Project lifecycle: load <project>/Library/PipelineCache.bin into the
        // live cache, and write the live cache back. Both are no-ops if no
        // project is currently loaded.
        static void LoadFromProject();
        static void SaveToProject();

        static VkPipelineCache Get() { return s_Cache; }

        // Compile-time instrumentation (perf observability). Every VKPipeline /
        // VKComputePipeline ctor records its vkCreate*Pipelines duration here;
        // a non-zero per-frame count in steady state is a PSO-compile hitch (cold cache miss).
        struct CompileStats { u32 count = 0; f64 totalMs = 0.0; f64 maxMs = 0.0; };
        static void RecordCompile(f64 ms);
        static CompileStats ConsumeFrameStats();   // returns + zeroes the per-frame accumulators

    private:
        static VkPipelineCache s_Cache;
    };
}
