#pragma once

#include "luth/core/LuthTypes.h"
#include "luth/renderer/rendergraph/RenderGraphResources.h"
#include <vulkan/vulkan.h>
#include <vector>

typedef struct VmaAllocation_T* VmaAllocation;

namespace Luth::RG
{
    struct PooledResource
    {
        VkImage image = VK_NULL_HANDLE;
        VkImageView view = VK_NULL_HANDLE;
        VmaAllocation allocation = nullptr;
        TextureDesc desc;
        u64 lastUsedFrame = 0;
    };

    class RenderResourceCache
    {
    public:
        void Init();
        void Shutdown();
        void NewFrame();

        void PerformGarbageCollection();

        PooledResource GetTexture(const TextureDesc& desc);
        void ReturnTexture(PooledResource resource);

    private:
        std::vector<PooledResource> m_Pool;
        u64 m_FrameIndex = 0;
        static constexpr u64 k_StaleFrameThreshold = 10000; // Frames before a resource is considered stale
    };
}
