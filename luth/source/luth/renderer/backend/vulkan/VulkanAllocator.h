#pragma once

#include "luth/core/types/LuthTypes.h"
#include <vulkan/vulkan.h>

// Forward declarations to keep header clean
typedef struct VmaAllocator_T* VmaAllocator;
typedef struct VmaAllocation_T* VmaAllocation;
enum VmaMemoryUsage;

namespace Luth
{
    // Static facade over VMA (Vulkan Memory Allocator). Owns the global VmaAllocator instance
    // and a small set of typed helpers for buffer / image allocation, persistently-mapped
    // sequential buffers (the GPU tagged-heap backing), and incremental flush. Allocations land
    // under MemoryTracker's Category::GPU bucket exactly once per backing.
    // GPU memory classification for the Profiler breakdown. Inferred from usage flags at alloc time;
    // overridable for the texture-vs-render-target split, which usage flags don't cleanly separate here
    // (this engine's sampled textures also carry COLOR_ATTACHMENT). Count doubles as the "auto-infer" arg.
    enum class GpuResourceClass : u8
    {
        Texture, RenderTarget, Mesh, Buffer, AccelStructure, Other, Count
    };

    struct GPUMemoryStats
    {
        u64 UsedBytes;
        u64 FreeBytes;
        u32 AllocationCount;
        u32 BlockCount;

        // Live bytes + allocation count per GpuResourceClass (indexed by the enum value).
        u64 ClassBytes[static_cast<u32>(GpuResourceClass::Count)];
        u32 ClassCount[static_cast<u32>(GpuResourceClass::Count)];
    };

    class VulkanAllocator
    {
    public:
        static void Init(VkInstance instance, VkPhysicalDevice physicalDevice, VkDevice device);
        static void Shutdown();

        static VmaAllocator Get();

        static VmaAllocation AllocateBuffer(const VkBufferCreateInfo& bufferInfo, VmaMemoryUsage usage, VkBuffer& outBuffer,
                                            GpuResourceClass cls = GpuResourceClass::Count);
        static VmaAllocation AllocateImage(const VkImageCreateInfo& imageInfo, VmaMemoryUsage usage, VkImage& outImage,
                                           GpuResourceClass cls = GpuResourceClass::Count);

        // Persistently-mapped sequential-write buffer for ring-style upload paths
        // (Material SSBO, ObjectSSBO, IndirectBuffer). Modern VMA: VMA_MEMORY_USAGE_AUTO
        // + HOST_ACCESS_SEQUENTIAL_WRITE_BIT + MAPPED_BIT; no separate Map() needed
        // (vmaDestroyBuffer auto-unmaps). HOST_COHERENT is not guaranteed — call
        // FlushSlice after writes. Sized for the caller; slice math is the caller's job.
        static VmaAllocation AllocateMappedSequentialBuffer(
            const VkBufferCreateInfo& bufferInfo,
            VkBuffer& outBuffer,
            void** outMappedData,
            GpuResourceClass cls = GpuResourceClass::Count);

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