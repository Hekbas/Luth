#pragma once

#include <vulkan/vulkan.h>

namespace Luth
{
    class PipelineCache
    {
    public:
        static void Init();
        static void Shutdown();
        static VkPipelineCache Get() { return s_Cache; }

    private:
        static VkPipelineCache s_Cache;
    };
}
