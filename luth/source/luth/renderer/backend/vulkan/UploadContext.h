#pragma once

#include "luth/core/types/LuthTypes.h"
#include "TimelineSemaphore.h"
#include <vulkan/vulkan.h>
#include <array>
#include <mutex>
#include <functional>
#include <vector>

// Forward declare VMA types
typedef struct VmaAllocation_T* VmaAllocation;

namespace Luth
{
    // Async data uploads on a dedicated transfer queue, backed by a staging ring buffer that avoids per-call VMA
    // allocations. UploadBuffer / UploadImage submit on the DMA-capable transfer queue (truly concurrent with
    // frame rendering on discrete GPUs); UploadImageMipped stays on the graphics queue because vkCmdBlitImage
    // requires VK_QUEUE_GRAPHICS_BIT per spec. Both share m_UploadTimeline; timeline semaphores accept multi-queue
    // signal, so DrainPendingBinds polls one value regardless of which queue signaled. See arch/multi-queue.md.
    // All three uploaders return a fence value the caller can poll if they need to gate texture binding on completion.

    class UploadContext
    {
    public:
        static void Init();
        static void Shutdown();
        static UploadContext& Get();

        // Returns a fence value that signals when the upload is complete.
        u64 UploadBuffer(const void* data, u64 size, VkBuffer dstBuffer, u64 dstOffset);

        // Returns a fence value that signals when the upload is complete.
        u64 UploadImage(const void* data, u64 size, VkImage dstImage, VkBufferImageCopy copyRegion);

        // Generates mips 1..N-1 via vkCmdBlitImage; caller passes mip-0 only.
        // BLIT_DST_BIT is a graphics-queue-only feature, so this stays on the graphics queue regardless of any future async-compute split.
        u64 UploadImageMipped(const void* data, u64 size, VkImage dstImage,
                              u32 width, u32 height, u32 mipLevels, u32 arrayLayers);

        // Uploads a pre-baked compressed (BCn) mip chain in one transfer submit: no blit (mips are
        // baked at import), so it runs on the transfer queue. Regions carry offsets relative to the
        // payload start; this rebases them onto the staging allocation.
        u64 UploadImageLevels(const void* data, u64 size, VkImage dstImage,
                              const std::vector<VkBufferImageCopy>& regions);

        // Checks if a specific upload has finished.
        bool IsComplete(u64 fenceValue);

        // Latest retired upload-timeline value. Sample once per pass and compare fences against it to gate a
        // hot loop (the draw list) without a timeline query per element.
        u64 CompletedUploadValue();

        // Blocks until a specific upload finishes; use sparingly.
        void WaitForUpload(u64 fenceValue);

        // Pending-bind pump: defers BindlessDescriptorSet::BindTexture until the upload fence retires.
        // outIndex must remain valid until DrainPendingBinds writes it OR CancelPendingBind removes the entry;
        // view is the cancel key (one-to-one with VKTexture, never reused across instances).
        void PushPendingBind(u32* outIndex, VkImageView view, VkSampler sampler, u64 fenceValue);
        void DrainPendingBinds();
        void CancelPendingBind(VkImageView view);

    private:
        void CreateResources();

        // Allocates space in the staging ring; blocks until the GPU catches up when full.
        u64 AllocateStaging(u64 size, u64 alignment, void** outMappedPtr, VkBuffer& outBuffer, u64& outOffset);

        // BeginRingSlot/RecordRingSlotFence pair must be called together inside m_Lock around the submit. The two
        // rings are independent (transfer-family pool feeds UploadBuffer/UploadImage on the transfer queue;
        // graphics-family pool feeds UploadImageMipped's blit chain on the graphics queue), but both signal the
        // same shared m_UploadTimeline so DrainPendingBinds doesn't care which queue retired the fence.
        VkCommandBuffer BeginTransferRingSlot();
        void RecordTransferRingSlotFence(u64 fenceValue);
        VkCommandBuffer BeginBlitRingSlot();
        void RecordBlitRingSlotFence(u64 fenceValue);

        // Submission-driven ring (not frame-scoped); uploads aren't frame-bounded.
        static constexpr u32 RING_SIZE = 4;

        // Transfer-queue ring: UploadBuffer + UploadImage (no blits). DMA-capable on discrete GPUs; aliases to
        // graphics on single-family hardware (Intel iGPU etc.) via VulkanContext queue discovery.
        VkQueue m_TransferQueue = VK_NULL_HANDLE;
        VkCommandPool m_CommandPool = VK_NULL_HANDLE;
        std::array<VkCommandBuffer, RING_SIZE> m_CmdRing{};
        std::array<u64, RING_SIZE> m_RingFenceValues{};
        u32 m_SubmitIndex = 0;

        // Graphics-queue ring: UploadImageMipped only; vkCmdBlitImage requires VK_QUEUE_GRAPHICS_BIT per spec
        // (VUID-vkCmdBlitImage-commandBuffer-cmdpool), so the mip-chain path can't run on the transfer queue.
        VkQueue m_GraphicsBlitQueue = VK_NULL_HANDLE;
        VkCommandPool m_GraphicsBlitPool = VK_NULL_HANDLE;
        std::array<VkCommandBuffer, RING_SIZE> m_BlitCmdRing{};
        std::array<u64, RING_SIZE> m_BlitRingFenceValues{};
        u32 m_BlitSubmitIndex = 0;

        // Synchronization
        TimelineSemaphore m_UploadTimeline;
        u64 m_CurrentValue = 0; // Latest signaled fence; Shutdown drains to here.

        // Staging Ring Buffer
        static constexpr u64 STAGING_SIZE = 64 * 1024 * 1024; // 64MB
        VkBuffer m_StagingBuffer = VK_NULL_HANDLE;
        VmaAllocation m_StagingAllocation = nullptr;
        void* m_StagingMapped = nullptr;
        
        u64 m_StagingHead = 0; // Next write offset
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
