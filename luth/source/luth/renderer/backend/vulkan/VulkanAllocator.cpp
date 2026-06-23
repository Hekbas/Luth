#include "luthpch.h"
#include "VulkanAllocator.h"
#include "luth/core/diagnostics/Log.h"
#include "luth/memory/MemoryTracker.h"

#define VMA_IMPLEMENTATION
#include <vma/vk_mem_alloc.h>

#include <atomic>
#include <cstdint>

namespace Luth
{
    struct AllocatorData
    {
        VmaAllocator allocator = nullptr;
    };

    static AllocatorData* s_Data = nullptr;

    namespace
    {
        constexpr u32 kClassCount = static_cast<u32>(GpuResourceClass::Count);

        // Live per-class bytes + counts. The class is stamped into each allocation's pUserData at create
        // time so FreeBuffer/FreeImage can decrement the right bucket without an external tracking map.
        std::atomic<u64> g_ClassBytes[kClassCount]{};
        std::atomic<u32> g_ClassCount[kClassCount]{};

        GpuResourceClass InferBufferClass(VkBufferUsageFlags u)
        {
            if (u & (VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR
                   | VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR))
                return GpuResourceClass::AccelStructure;
            if (u & (VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT))
                return GpuResourceClass::Mesh;
            return GpuResourceClass::Buffer;
        }

        GpuResourceClass InferImageClass(VkImageUsageFlags u)
        {
            const bool attachment = (u & (VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT
                                        | VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT)) != 0;
            // Asset textures are uploaded (TRANSFER_DST) yet also carry COLOR_ATTACHMENT here; render-graph
            // targets are rendered into, not uploaded — so TRANSFER_DST is the texture-vs-target discriminator.
            if (attachment && !(u & VK_IMAGE_USAGE_TRANSFER_DST_BIT)) return GpuResourceClass::RenderTarget;
            if (u & VK_IMAGE_USAGE_SAMPLED_BIT)                       return GpuResourceClass::Texture;
            if (attachment || (u & VK_IMAGE_USAGE_STORAGE_BIT))       return GpuResourceClass::RenderTarget;
            return GpuResourceClass::Other;
        }

        void* ClassTag(GpuResourceClass cls) { return reinterpret_cast<void*>(static_cast<uintptr_t>(cls)); }

        void RecordClass(GpuResourceClass cls, u64 size)
        {
            u32 i = static_cast<u32>(cls);
            if (i >= kClassCount) i = static_cast<u32>(GpuResourceClass::Other);
            g_ClassBytes[i].fetch_add(size, std::memory_order_relaxed);
            g_ClassCount[i].fetch_add(1, std::memory_order_relaxed);
        }

        void UnrecordClass(void* userData, u64 size)
        {
            u32 i = static_cast<u32>(reinterpret_cast<uintptr_t>(userData));
            if (i >= kClassCount) i = static_cast<u32>(GpuResourceClass::Other);
            g_ClassBytes[i].fetch_sub(size, std::memory_order_relaxed);
            g_ClassCount[i].fetch_sub(1, std::memory_order_relaxed);
        }
    }

    void VulkanAllocator::Init(VkInstance instance, VkPhysicalDevice physicalDevice, VkDevice device)
    {
        s_Data = LH_NEW(Memory::Category::Rendering, AllocatorData);

        VmaAllocatorCreateInfo allocatorInfo = {};
        allocatorInfo.physicalDevice = physicalDevice;
        allocatorInfo.device = device;
        allocatorInfo.instance = instance;
        allocatorInfo.vulkanApiVersion = VK_API_VERSION_1_3;
        allocatorInfo.flags = VMA_ALLOCATOR_CREATE_BUFFER_DEVICE_ADDRESS_BIT;

        if (vmaCreateAllocator(&allocatorInfo, &s_Data->allocator) != VK_SUCCESS) {
            LH_CORE_CRITICAL("Failed to create VMA allocator!");
        }
    }

    void VulkanAllocator::Shutdown()
    {
        if (s_Data && s_Data->allocator) {
            vmaDestroyAllocator(s_Data->allocator);
            LH_DELETE(Memory::Category::Rendering, s_Data);
        }
    }

    VmaAllocator VulkanAllocator::Get()
    {
        return s_Data->allocator;
    }

    VmaAllocation VulkanAllocator::AllocateBuffer(const VkBufferCreateInfo& bufferInfo, VmaMemoryUsage usage, VkBuffer& outBuffer, GpuResourceClass cls)
    {
        if (cls == GpuResourceClass::Count) cls = InferBufferClass(bufferInfo.usage);

        VmaAllocationCreateInfo allocInfo = {};
        allocInfo.usage = usage;
        allocInfo.pUserData = ClassTag(cls);

        VmaAllocation allocation;
        vmaCreateBuffer(s_Data->allocator, &bufferInfo, &allocInfo, &outBuffer, &allocation, nullptr);

        VmaAllocationInfo vmaAllocInfo;
        vmaGetAllocationInfo(s_Data->allocator, allocation, &vmaAllocInfo);
        Memory::MemoryTracker::RecordAlloc(Memory::Category::GPU, vmaAllocInfo.size);
        RecordClass(cls, vmaAllocInfo.size);

        return allocation;
    }

    VmaAllocation VulkanAllocator::AllocateMappedSequentialBuffer(
        const VkBufferCreateInfo& bufferInfo,
        VkBuffer& outBuffer,
        void** outMappedData,
        GpuResourceClass cls)
    {
        if (cls == GpuResourceClass::Count) cls = InferBufferClass(bufferInfo.usage);

        VmaAllocationCreateInfo allocInfo = {};
        allocInfo.usage = VMA_MEMORY_USAGE_AUTO;
        allocInfo.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT
                        | VMA_ALLOCATION_CREATE_MAPPED_BIT;
        allocInfo.pUserData = ClassTag(cls);

        VmaAllocation allocation;
        VmaAllocationInfo allocInfoOut{};
        vmaCreateBuffer(s_Data->allocator, &bufferInfo, &allocInfo, &outBuffer, &allocation, &allocInfoOut);

        Memory::MemoryTracker::RecordAlloc(Memory::Category::GPU, allocInfoOut.size);
        RecordClass(cls, allocInfoOut.size);

        if (outMappedData)
            *outMappedData = allocInfoOut.pMappedData;

        return allocation;
    }

    void VulkanAllocator::FlushSlice(VmaAllocation allocation, VkDeviceSize offset, VkDeviceSize size)
    {
        // No-op on HOST_COHERENT memory types (VMA inspects the alloc's memory
        // properties internally), so callers don't need a coherence cache.
        vmaFlushAllocation(s_Data->allocator, allocation, offset, size);
    }

    VmaAllocation VulkanAllocator::AllocateImage(const VkImageCreateInfo& imageInfo, VmaMemoryUsage usage, VkImage& outImage, GpuResourceClass cls)
    {
        if (cls == GpuResourceClass::Count) cls = InferImageClass(imageInfo.usage);

        VmaAllocationCreateInfo allocInfo = {};
        allocInfo.usage = usage;
        allocInfo.pUserData = ClassTag(cls);

        VmaAllocation allocation;
        vmaCreateImage(s_Data->allocator, &imageInfo, &allocInfo, &outImage, &allocation, nullptr);

        VmaAllocationInfo vmaAllocInfo;
        vmaGetAllocationInfo(s_Data->allocator, allocation, &vmaAllocInfo);
        Memory::MemoryTracker::RecordAlloc(Memory::Category::GPU, vmaAllocInfo.size);
        RecordClass(cls, vmaAllocInfo.size);

        LH_CORE_TRACE("VMA Alloc Image: {0}x{1}", imageInfo.extent.width, imageInfo.extent.height);

        return allocation;
    }

    void VulkanAllocator::FreeBuffer(VkBuffer buffer, VmaAllocation allocation) {
        VmaAllocationInfo vmaAllocInfo;
        vmaGetAllocationInfo(s_Data->allocator, allocation, &vmaAllocInfo);
        Memory::MemoryTracker::RecordFree(Memory::Category::GPU, vmaAllocInfo.size);
        UnrecordClass(vmaAllocInfo.pUserData, vmaAllocInfo.size);

        vmaDestroyBuffer(s_Data->allocator, buffer, allocation);
    }

    void VulkanAllocator::FreeImage(VkImage image, VmaAllocation allocation) {
        VmaAllocationInfo vmaAllocInfo;
        vmaGetAllocationInfo(s_Data->allocator, allocation, &vmaAllocInfo);
        Memory::MemoryTracker::RecordFree(Memory::Category::GPU, vmaAllocInfo.size);
        UnrecordClass(vmaAllocInfo.pUserData, vmaAllocInfo.size);

        LH_CORE_TRACE("VMA Free Image");
        vmaDestroyImage(s_Data->allocator, image, allocation);
    }

    void* VulkanAllocator::Map(VmaAllocation allocation) {
        void* data;
        vmaMapMemory(s_Data->allocator, allocation, &data);
        return data;
    }

    void VulkanAllocator::Unmap(VmaAllocation allocation) {
        vmaUnmapMemory(s_Data->allocator, allocation);
    }

    GPUMemoryStats VulkanAllocator::GetStats()
    {
        GPUMemoryStats stats = {};
        if (s_Data && s_Data->allocator)
        {
            VmaTotalStatistics vmaStats;
            vmaCalculateStatistics(s_Data->allocator, &vmaStats);
            stats.UsedBytes = vmaStats.total.statistics.allocationBytes;
            stats.FreeBytes = vmaStats.total.statistics.blockBytes - vmaStats.total.statistics.allocationBytes;
            stats.AllocationCount = vmaStats.total.statistics.allocationCount;
            stats.BlockCount = vmaStats.total.statistics.blockCount;
        }
        for (u32 i = 0; i < kClassCount; ++i)
        {
            stats.ClassBytes[i] = g_ClassBytes[i].load(std::memory_order_relaxed);
            stats.ClassCount[i] = g_ClassCount[i].load(std::memory_order_relaxed);
        }
        return stats;
    }
}
