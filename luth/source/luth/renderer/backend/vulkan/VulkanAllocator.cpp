#include "luthpch.h"
#include "VulkanAllocator.h"
#include "luth/core/Log.h"

#define VMA_IMPLEMENTATION
#include <vma/vk_mem_alloc.h>

namespace Luth
{
    struct AllocatorData
    {
        VmaAllocator allocator = nullptr;
    };

    static AllocatorData* s_Data = nullptr;

    void VulkanAllocator::Init(VkInstance instance, VkPhysicalDevice physicalDevice, VkDevice device)
    {
        s_Data = new AllocatorData();

        VmaAllocatorCreateInfo allocatorInfo = {};
        allocatorInfo.physicalDevice = physicalDevice;
        allocatorInfo.device = device;
        allocatorInfo.instance = instance;
        allocatorInfo.vulkanApiVersion = VK_API_VERSION_1_3;

        if (vmaCreateAllocator(&allocatorInfo, &s_Data->allocator) != VK_SUCCESS) {
            LH_CORE_CRITICAL("Failed to create VMA allocator!");
        }
    }

    void VulkanAllocator::Shutdown()
    {
        if (s_Data && s_Data->allocator) {
            vmaDestroyAllocator(s_Data->allocator);
            delete s_Data;
            s_Data = nullptr;
        }
    }

    VmaAllocator VulkanAllocator::Get()
    {
        return s_Data->allocator;
    }

    VmaAllocation VulkanAllocator::AllocateBuffer(const VkBufferCreateInfo& bufferInfo, VmaMemoryUsage usage, VkBuffer& outBuffer)
    {
        VmaAllocationCreateInfo allocInfo = {};
        allocInfo.usage = usage;

        VmaAllocation allocation;
        vmaCreateBuffer(s_Data->allocator, &bufferInfo, &allocInfo, &outBuffer, &allocation, nullptr);
        return allocation;
    }

    VmaAllocation VulkanAllocator::AllocateImage(const VkImageCreateInfo& imageInfo, VmaMemoryUsage usage, VkImage& outImage)
    {
        VmaAllocationCreateInfo allocInfo = {};
        allocInfo.usage = usage;

        VmaAllocation allocation;
        vmaCreateImage(s_Data->allocator, &imageInfo, &allocInfo, &outImage, &allocation, nullptr);
        
        LH_CORE_TRACE("VMA Alloc Image: {0}x{1}", imageInfo.extent.width, imageInfo.extent.height);
        
        return allocation;
    }

    void VulkanAllocator::FreeBuffer(VkBuffer buffer, VmaAllocation allocation) {
        vmaDestroyBuffer(s_Data->allocator, buffer, allocation);
    }

    void VulkanAllocator::FreeImage(VkImage image, VmaAllocation allocation) {
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
        return stats;
    }
}
