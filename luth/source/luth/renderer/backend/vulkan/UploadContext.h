#pragma once

#include "luth/core/LuthTypes.h"
#include "TimelineSemaphore.h"
#include <vulkan/vulkan.h>
#include <mutex>
#include <functional>

// Forward declare VMA types
typedef struct VmaAllocation_T* VmaAllocation;

namespace Luth
{
    // ===================================================================================
    // Upload Context (Async Transfer Queue)
    // ===================================================================================
    // Handles asynchronous data uploads to the GPU using a dedicated Transfer Queue.
    // Uses a Ring Buffer for staging memory to avoid frequent allocations.
    
    class UploadContext
    {
    public:
        static void Init();
        static void Shutdown();
        static UploadContext& Get();

        // Uploads data to a buffer.
        // Returns a fence value that signals when the upload is complete.
        u64 UploadBuffer(const void* data, u64 size, VkBuffer dstBuffer, u64 dstOffset);

        // Uploads data to an image.
        // Returns a fence value that signals when the upload is complete.
        u64 UploadImage(const void* data, u64 size, VkImage dstImage, VkBufferImageCopy copyRegion);

        // Checks if a specific upload has finished.
        bool IsComplete(u64 fenceValue);

        // Waits for a specific upload to finish (Blocking - Use sparingly!).
        void WaitForUpload(u64 fenceValue);

    private:
        void CreateResources();
        
        // Allocates space in the ring buffer.
        // If full, it waits for the GPU to catch up.
        u64 AllocateStaging(u64 size, u64 alignment, void** outMappedPtr, VkBuffer& outBuffer, u64& outOffset);

        VkQueue m_TransferQueue = VK_NULL_HANDLE;
        VkCommandPool m_CommandPool = VK_NULL_HANDLE;
        VkCommandBuffer m_CommandBuffer = VK_NULL_HANDLE;
        
        // Synchronization
        TimelineSemaphore m_UploadTimeline;
        u64 m_CurrentValue = 0; // Value signaled by the last submitted batch
        u64 m_SubmittedValue = 0; // Value of the currently recording batch

        // Staging Ring Buffer
        static constexpr u64 STAGING_SIZE = 64 * 1024 * 1024; // 64MB
        VkBuffer m_StagingBuffer = VK_NULL_HANDLE;
        VmaAllocation m_StagingAllocation = nullptr;
        void* m_StagingMapped = nullptr;
        
        u64 m_StagingHead = 0; // Where we write next
        u64 m_StagingTail = 0; // Oldest data still in use by GPU (tracked by fence)
        
        // Track fence values for ring buffer regions
        struct StagingBlock
        {
            u64 offset;
            u64 size;
            u64 fenceValue;
        };
        std::vector<StagingBlock> m_InFlightBlocks;

        std::mutex m_Lock;
    };
}
