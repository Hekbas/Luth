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
    // Async data uploads on a dedicated transfer queue, backed by a staging ring buffer that avoids
    // per-call VMA allocations. UploadBuffer / UploadImage / UploadImageMipped return a fence value
    // the caller can poll if they need to gate texture binding on completion.

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

        // Generates mips 1..N-1 via vkCmdBlitImage; caller passes mip-0 only.
        // BLIT_DST_BIT is a graphics-queue-only feature, so this stays on the graphics queue regardless of any future async-compute split.
        u64 UploadImageMipped(const void* data, u64 size, VkImage dstImage,
                              u32 width, u32 height, u32 mipLevels, u32 arrayLayers);

        // Checks if a specific upload has finished.
        bool IsComplete(u64 fenceValue);

        // Waits for a specific upload to finish (Blocking - Use sparingly!).
        void WaitForUpload(u64 fenceValue);

        // Pending-bind pump — defers BindlessDescriptorSet::BindTexture until the upload fence retires.
        // outIndex must remain valid until DrainPendingBinds writes it OR CancelPendingBind removes the entry;
        // view is the cancel key (one-to-one with VKTexture, never reused across instances).
        void PushPendingBind(u32* outIndex, VkImageView view, VkSampler sampler, u64 fenceValue);
        void DrainPendingBinds();
        void CancelPendingBind(VkImageView view);

    private:
        void CreateResources();

        // Allocates space in the ring buffer.
        // If full, it waits for the GPU to catch up.
        u64 AllocateStaging(u64 size, u64 alignment, void** outMappedPtr, VkBuffer& outBuffer, u64& outOffset);

        // BeginRingSlot/RecordRingSlotFence pair must be called together inside m_Lock around the submit.
        VkCommandBuffer BeginRingSlot();
        void RecordRingSlotFence(u64 fenceValue);

        // Submission-driven ring (not frame-scoped) — uploads aren't frame-bounded.
        static constexpr u32 RING_SIZE = 4;

        VkQueue m_TransferQueue = VK_NULL_HANDLE;
        VkCommandPool m_CommandPool = VK_NULL_HANDLE;
        std::array<VkCommandBuffer, RING_SIZE> m_CmdRing{};
        std::array<u64, RING_SIZE> m_RingFenceValues{};
        u32 m_SubmitIndex = 0;

        // Synchronization
        TimelineSemaphore m_UploadTimeline;
        u64 m_CurrentValue = 0; // Latest signaled fence; Shutdown drains to here.

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

        struct PendingBind
        {
            u32* outIndex;     // points into VKTexture::m_BindlessIndex
            VkImageView view;  // unique per VKTexture; used as cancel key
            VkSampler sampler;
            u64 fenceValue;
        };
        std::vector<PendingBind> m_PendingBinds;

        std::mutex m_Lock;
    };
}
