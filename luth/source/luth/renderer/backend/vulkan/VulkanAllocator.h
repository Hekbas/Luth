#pragma once

#include "luth/core/types/LuthTypes.h"
#include <vulkan/vulkan.h>

// Forward declarations to keep header clean
typedef struct VmaAllocator_T* VmaAllocator;
typedef struct VmaAllocation_T* VmaAllocation;
//Allowed in MSVC not in GCC/Clang, so we should maybe use /permissive-
//enum VmaMemoryUsage;
#include "vma/vk_mem_alloc.h"

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

        // Persistently-mapped sequential-write buffer for ring-style upload paths
        // (Material SSBO, ObjectSSBO, IndirectBuffer). Modern VMA: VMA_MEMORY_USAGE_AUTO
        // + HOST_ACCESS_SEQUENTIAL_WRITE_BIT + MAPPED_BIT; no separate Map() needed
        // (vmaDestroyBuffer auto-unmaps). HOST_COHERENT is not guaranteed — call
        // FlushSlice after writes. Sized for the caller; slice math is the caller's job.
        static VmaAllocation AllocateMappedSequentialBuffer(
            const VkBufferCreateInfo& bufferInfo,
            VkBuffer& outBuffer,
            void** outMappedData);

        // Flushes a sub-range of a HOST_VISIBLE allocation. No-op when the underlying
        // memory type is HOST_COHERENT (vmaFlushAllocation handles the gating).
        static void FlushSlice(VmaAllocation allocation, VkDeviceSize offset, VkDeviceSize size);

        static void FreeBuffer(VkBuffer buffer, VmaAllocation allocation);
        static void FreeImage(VkImage image, VmaAllocation allocation);

        static void* Map(VmaAllocation allocation);
        static void Unmap(VmaAllocation allocation);

        static GPUMemoryStats GetStats();
    };
}