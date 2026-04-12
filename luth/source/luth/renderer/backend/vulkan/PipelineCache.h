#pragma once

#include <vulkan/vulkan.h>

namespace Luth
{
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

    private:
        static VkPipelineCache s_Cache;
    };
}
