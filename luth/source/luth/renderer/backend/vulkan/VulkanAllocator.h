#pragma once

#include "luth/core/LuthTypes.h"
#include <vulkan/vulkan.h>

// Forward declarations to keep header clean
typedef struct VmaAllocator_T* VmaAllocator;
typedef struct VmaAllocation_T* VmaAllocation;
enum VmaMemoryUsage;

namespace Luth
{
    struct GPUMemoryStats
    {
        u64 UsedBytes;
        u64 FreeBytes;
        u32 AllocationCount;
        u32 BlockCount;
    };

    class VulkanAllocator
    {
    public:
        static void Init(VkInstance instance, VkPhysicalDevice physicalDevice, VkDevice device);
        static void Shutdown();

        static VmaAllocator Get();

        static VmaAllocation AllocateBuffer(const VkBufferCreateInfo& bufferInfo, VmaMemoryUsage usage, VkBuffer& outBuffer);
        static VmaAllocation AllocateImage(const VkImageCreateInfo& imageInfo, VmaMemoryUsage usage, VkImage& outImage);
        
        static void FreeBuffer(VkBuffer buffer, VmaAllocation allocation);
        static void FreeImage(VkImage image, VmaAllocation allocation);

        static void* Map(VmaAllocation allocation);
        static void Unmap(VmaAllocation allocation);

        static GPUMemoryStats GetStats();
    };
}