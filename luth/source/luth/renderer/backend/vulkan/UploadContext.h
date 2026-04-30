#pragma once

#include "luth/core/types/LuthTypes.h"
#include "TimelineSemaphore.h"
#include <vulkan/vulkan.h>
#include <array>
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

        // Uploads mip-0 data to a color image and generates mips 1..N-1 via vkCmdBlitImage.
        // `data` is mip-0 only; `size` is mip-0 size in bytes. Aspect is COLOR; final layout is SHADER_READ_ONLY across all mips.
        // Caller must ensure `format` supports BLIT_SRC|BLIT_DST when mipLevels > 1 (graphics queue forever per VK_FORMAT_FEATURE_BLIT_DST_BIT).
        u64 UploadImageMipped(const void* data, u64 size, VkImage dstImage,
                              u32 width, u32 height, u32 mipLevels, u32 arrayLayers);

        // Checks if a specific upload has finished.
        bool IsComplete(u64 fenceValue);

        // Waits for a specific upload to finish (Blocking - Use sparingly!).
        void WaitForUpload(u64 fenceValue);

    private:
        void CreateResources();

        // Allocates space in the ring buffer.
        // If full, it waits for the GPU to catch up.
        u64 AllocateStaging(u64 size, u64 alignment, void** outMappedPtr, VkBuffer& outBuffer, u64& outOffset);

        // Acquires the next cmd-buffer ring slot. Waits only if that slot's last fence hasn't retired.
        // Caller must invoke RecordRingSlotFence(fenceValue) after submit. Both must be inside m_Lock.
        VkCommandBuffer BeginRingSlot();
        void RecordRingSlotFence(u64 fenceValue);

        // Cmd-buffer ring — submission-driven (not frame-scoped). Independent of MAX_FRAMES_IN_FLIGHT.
        // Sized to cover tight back-to-back uploads before staging-ring saturation forces a fence wait anyway.
        static constexpr u32 RING_SIZE = 4;

        VkQueue m_TransferQueue = VK_NULL_HANDLE;
        VkCommandPool m_CommandPool = VK_NULL_HANDLE;
        std::array<VkCommandBuffer, RING_SIZE> m_CmdRing{};
        std::array<u64, RING_SIZE> m_RingFenceValues{};
        u32 m_SubmitIndex = 0; // Round-robin index; current slot = m_SubmitIndex % RING_SIZE

        // Synchronization
        TimelineSemaphore m_UploadTimeline;
        u64 m_CurrentValue = 0; // Value signaled by the last submitted batch (Shutdown drains to here)

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
