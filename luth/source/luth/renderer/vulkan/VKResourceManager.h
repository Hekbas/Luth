#pragma once

#include "luth/renderer/rendergraph/RenderGraphResources.h"
#include "luth/renderer/vulkan/VKDevice.h"
#include <vulkan/vulkan.h>
#include <unordered_map>
#include <vector>
#include <queue>

namespace Luth
{
    struct VKImageResource
    {
        VkImage image = VK_NULL_HANDLE;
        VkImageView view = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        VkFormat format;
        VkExtent3D extent;
        
        // Debug name
        std::string name;
    };

    class VKResourceManager
    {
    public:
        VKResourceManager(VkDevice device, VkPhysicalDevice physicalDevice);
        ~VKResourceManager();

        // Request a texture. If a matching one exists in the pool, it's returned.
        // Otherwise, a new one is created.
        VKImageResource* GetTexture(const RG::TextureDesc& desc);

        // Mark a texture as unused so it can be returned to the pool next frame.
        void ReleaseTexture(VKImageResource* resource);

        // Clean up old resources (call at start of frame)
        void GarbageCollect();

    private:
        VkFormat ConvertFormat(RG::TextureFormat format);
        void CreateImage(const RG::TextureDesc& desc, VKImageResource& outResource);

        VkDevice m_Device;
        VkPhysicalDevice m_PhysicalDevice;

        // Simple pooling: Map hash(desc) -> List of available resources
        // For a robust engine, we'd use a proper cache with LRU eviction.
        struct PoolEntry
        {
            VKImageResource resource;
            u64 lastUsedFrame = 0;
        };
        
        std::unordered_map<u64, std::vector<PoolEntry>> m_TexturePool;
        u64 m_CurrentFrame = 0;
    };
}
