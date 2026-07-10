#include "luthpch.h"
#include "UploadContext.h"
#include "VulkanContext.h"
#include "VulkanAllocator.h"
#include "VulkanDescriptors.h"
#include "luth/core/diagnostics/Log.h"
#include "luth/core/diagnostics/Profiler.h"

#include <vma/vk_mem_alloc.h>

namespace Luth
{
    static UploadContext* s_Instance = nullptr;

    void UploadContext::Init()
    {
        if (s_Instance) return;
        s_Instance = LH_NEW(Memory::Category::Rendering, UploadContext);
        s_Instance->CreateResources();
        LH_LOG(Renderer, info, "Upload Context Initialized (Staging: 64MB)");
    }

    void UploadContext::Shutdown()
    {
        if (!s_Instance) return;
        LH_PROFILE_FUNCTION();

        VkDevice device = VulkanContext::Get().GetDevice();

        // Wait for all uploads; m_CurrentValue is the highest fence value signaled by either ring.
        s_Instance->m_UploadTimeline.Wait(s_Instance->m_CurrentValue);

        s_Instance->m_UploadTimeline.Shutdown();
        vkDestroyCommandPool(device, s_Instance->m_CommandPool, nullptr);
        vkDestroyCommandPool(device, s_Instance->m_GraphicsBlitPool, nullptr);

        VulkanAllocator::Unmap(s_Instance->m_StagingAllocation);
        VulkanAllocator::FreeBuffer(s_Instance->m_StagingBuffer, s_Instance->m_StagingAllocation);

        LH_DELETE(Memory::Category::Rendering, s_Instance);
    }

    UploadContext& UploadContext::Get()
    {
        LH_CORE_ASSERT(s_Instance, "UploadContext not initialized!");
        return *s_Instance;
    }

    void UploadContext::CreateResources()
    {
        LH_PROFILE_FUNCTION();

        VkDevice device = VulkanContext::Get().GetDevice();

        // Transfer ring on the DMA-capable family (aliased to graphics on single-family GPUs). UploadBuffer +
        // UploadImage submit here so plain copies run concurrently with frame rendering on discrete hardware.
        m_TransferQueue = VulkanContext::Get().GetTransferQueue();
        const u32 transferFamily = VulkanContext::Get().GetTransferFamily();

        VkCommandPoolCreateInfo poolInfo{};
        poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        poolInfo.queueFamilyIndex = transferFamily;
        poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        vkCreateCommandPool(device, &poolInfo, nullptr, &m_CommandPool);

        VkCommandBufferAllocateInfo allocInfo{};
        allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        allocInfo.commandPool = m_CommandPool;
        allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        allocInfo.commandBufferCount = RING_SIZE;
        vkAllocateCommandBuffers(device, &allocInfo, m_CmdRing.data());

        // Graphics-blit ring on the graphics family: vkCmdBlitImage requires VK_QUEUE_GRAPHICS_BIT per spec, so
        // UploadImageMipped's mip-chain path can't share the transfer ring. Even on single-family GPUs the second
        // pool is harmless duplication; with a distinct transfer family it runs concurrently with frame work.
        m_GraphicsBlitQueue = VulkanContext::Get().GetGraphicsQueue();
        VkCommandPoolCreateInfo blitPoolInfo{};
        blitPoolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        blitPoolInfo.queueFamilyIndex = VulkanContext::Get().GetGraphicsFamily();
        blitPoolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        vkCreateCommandPool(device, &blitPoolInfo, nullptr, &m_GraphicsBlitPool);

        VkCommandBufferAllocateInfo blitAllocInfo{};
        blitAllocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        blitAllocInfo.commandPool = m_GraphicsBlitPool;
        blitAllocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        blitAllocInfo.commandBufferCount = RING_SIZE;
        vkAllocateCommandBuffers(device, &blitAllocInfo, m_BlitCmdRing.data());

        // Shared timeline: both rings signal monotonically increasing values; pump polls GetValue() agnostic of
        // which queue retired. Vulkan timeline semaphores explicitly support multi-queue signal.
        m_UploadTimeline.Init(0);

        VkBufferCreateInfo bufferInfo = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
        bufferInfo.size = STAGING_SIZE;
        bufferInfo.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
        // Staging is read by transfer queue only; CONCURRENT not required (and would broaden the family list
        // unnecessarily). Stays EXCLUSIVE per the per-resource opt-in policy.
        m_StagingAllocation = VulkanAllocator::AllocateBuffer(bufferInfo, VMA_MEMORY_USAGE_CPU_ONLY, m_StagingBuffer);
        m_StagingMapped = VulkanAllocator::Map(m_StagingAllocation);
    }

    VkCommandBuffer UploadContext::BeginTransferRingSlot()
    {
        LH_PROFILE_FUNCTION();

        const u32 slot = m_SubmitIndex % RING_SIZE;
        const u64 oldFence = m_RingFenceValues[slot];

        // Wait only if this slot's previous submission is still in flight; submits on different slots overlap on
        // the GPU. The staging ring typically saturates first in practice.
        if (oldFence > 0 && m_UploadTimeline.GetValue() < oldFence)
            m_UploadTimeline.Wait(oldFence);

        VkCommandBuffer cmd = m_CmdRing[slot];
        vkResetCommandBuffer(cmd, 0);
        return cmd;
    }

    void UploadContext::RecordTransferRingSlotFence(u64 fenceValue)
    {
        const u32 slot = m_SubmitIndex % RING_SIZE;
        m_RingFenceValues[slot] = fenceValue;
        m_SubmitIndex++;
    }

    VkCommandBuffer UploadContext::BeginBlitRingSlot()
    {
        LH_PROFILE_FUNCTION();

        const u32 slot = m_BlitSubmitIndex % RING_SIZE;
        const u64 oldFence = m_BlitRingFenceValues[slot];
        if (oldFence > 0 && m_UploadTimeline.GetValue() < oldFence)
            m_UploadTimeline.Wait(oldFence);
        VkCommandBuffer cmd = m_BlitCmdRing[slot];
        vkResetCommandBuffer(cmd, 0);
        return cmd;
    }

    void UploadContext::RecordBlitRingSlotFence(u64 fenceValue)
    {
        const u32 slot = m_BlitSubmitIndex % RING_SIZE;
        m_BlitRingFenceValues[slot] = fenceValue;
        m_BlitSubmitIndex++;
    }

    u64 UploadContext::AllocateStaging(u64 size, u64 alignment, void** outMappedPtr, VkBuffer& outBuffer, u64& outOffset)
    {
        LH_PROFILE_FUNCTION();

        LH_CORE_ASSERT(size <= STAGING_SIZE, "AllocateStaging: size exceeds ring capacity");

        u64 alignedHead = (m_StagingHead + (alignment - 1)) & ~(alignment - 1);

        // invariant: after wrap, the ring is in "wrapped" state for *this* call:
        // tail is downstream of head, must not be crossed. Re-deriving from
        // alignedHead < m_StagingTail post-tail-update misclassifies the case
        // where the oldest in-flight block sits at offset 0.
        bool wrapped = false;
        if (alignedHead + size > STAGING_SIZE)
        {
            alignedHead = 0;
            m_StagingHead = 0;
            wrapped = true;
        }

        u64 completedValue = m_UploadTimeline.GetValue();
        while (!m_InFlightBlocks.empty() && m_InFlightBlocks.front().fenceValue <= completedValue)
            m_InFlightBlocks.erase(m_InFlightBlocks.begin());

        if (!m_InFlightBlocks.empty())
            m_StagingTail = m_InFlightBlocks.front().offset;

        bool hasSpace = false;
        if (m_InFlightBlocks.empty())
        {
            hasSpace = true;
        }
        else if (alignedHead == m_StagingTail)
        {
            // ring full: head landed exactly on the oldest in-flight block's start
            // (happens when a wrap-allocation ended at the tail block's offset).
            hasSpace = false;
        }
        else if (wrapped || alignedHead < m_StagingTail)
        {
            // wrapped: free region is [alignedHead, m_StagingTail)
            hasSpace = (alignedHead + size <= m_StagingTail);
        }
        else
        {
            // head past tail; wrap check above already verified alignedHead+size <= STAGING_SIZE
            hasSpace = true;
        }

        if (!hasSpace)
        {
            LH_CORE_ASSERT(!m_InFlightBlocks.empty(), "AllocateStaging: out of space with no in-flight work");
            m_UploadTimeline.Wait(m_InFlightBlocks.front().fenceValue);
            return AllocateStaging(size, alignment, outMappedPtr, outBuffer, outOffset);
        }

        *outMappedPtr = (u8*)m_StagingMapped + alignedHead;
        outBuffer = m_StagingBuffer;
        outOffset = alignedHead;
        m_StagingHead = alignedHead + size;

        return m_CurrentValue + 1;
    }

    u64 UploadContext::UploadBuffer(const void* data, u64 size, VkBuffer dstBuffer, u64 dstOffset)
    {
        std::lock_guard<std::mutex> lock(m_Lock);
        LH_PROFILE_FUNCTION();

        void* stagingPtr;
        VkBuffer stagingBuffer;
        u64 stagingOffset;

        u64 fenceValue = AllocateStaging(size, 4, &stagingPtr, stagingBuffer, stagingOffset);

        memcpy(stagingPtr, data, size);

        VkCommandBuffer cmd = BeginTransferRingSlot();

        VkCommandBufferBeginInfo beginInfo{};
        beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkBeginCommandBuffer(cmd, &beginInfo);

        VkBufferCopy copyRegion{};
        copyRegion.srcOffset = stagingOffset;
        copyRegion.dstOffset = dstOffset;
        copyRegion.size = size;
        vkCmdCopyBuffer(cmd, stagingBuffer, dstBuffer, 1, &copyRegion);

        vkEndCommandBuffer(cmd);

        VkSemaphoreSubmitInfo signalInfo{ VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO };
        signalInfo.semaphore = m_UploadTimeline.GetHandle();
        signalInfo.value     = fenceValue;
        signalInfo.stageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
        VkCommandBufferSubmitInfo cmdInfo{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO };
        cmdInfo.commandBuffer = cmd;
        VkSubmitInfo2 submitInfo{ VK_STRUCTURE_TYPE_SUBMIT_INFO_2 };
        submitInfo.commandBufferInfoCount   = 1;
        submitInfo.pCommandBufferInfos      = &cmdInfo;
        submitInfo.signalSemaphoreInfoCount = 1;
        submitInfo.pSignalSemaphoreInfos    = &signalInfo;
        VulkanContext::Get().SubmitTransfer2(submitInfo, VK_NULL_HANDLE);

        m_CurrentValue = fenceValue;
        m_InFlightBlocks.push_back({ stagingOffset, size, fenceValue });
        RecordTransferRingSlotFence(fenceValue);

        return fenceValue;
    }

    u64 UploadContext::UploadImage(const void* data, u64 size, VkImage dstImage, VkBufferImageCopy copyRegion)
    {
        std::lock_guard<std::mutex> lock(m_Lock);
        LH_PROFILE_FUNCTION();

        void* stagingPtr;
        VkBuffer stagingBuffer;
        u64 stagingOffset;

        u64 fenceValue = AllocateStaging(size, 4, &stagingPtr, stagingBuffer, stagingOffset);

        memcpy(stagingPtr, data, size);

        copyRegion.bufferOffset += stagingOffset;

        VkCommandBuffer cmd = BeginTransferRingSlot();

        VkCommandBufferBeginInfo beginInfo{};
        beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkBeginCommandBuffer(cmd, &beginInfo);

        // Pre-barrier: UNDEFINED -> TRANSFER_DST. Sync2 + transfer-queue-compatible stages only.
        VkImageMemoryBarrier2 pre{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2 };
        pre.oldLayout           = VK_IMAGE_LAYOUT_UNDEFINED;
        pre.newLayout           = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        pre.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        pre.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        pre.image               = dstImage;
        pre.subresourceRange.aspectMask     = copyRegion.imageSubresource.aspectMask;
        pre.subresourceRange.baseMipLevel   = copyRegion.imageSubresource.mipLevel;
        pre.subresourceRange.levelCount     = 1;
        pre.subresourceRange.baseArrayLayer = copyRegion.imageSubresource.baseArrayLayer;
        pre.subresourceRange.layerCount     = copyRegion.imageSubresource.layerCount;
        pre.srcStageMask  = VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT;
        pre.srcAccessMask = VK_ACCESS_2_NONE;
        pre.dstStageMask  = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
        pre.dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        VkDependencyInfo preDep{ VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
        preDep.imageMemoryBarrierCount = 1;
        preDep.pImageMemoryBarriers    = &pre;
        vkCmdPipelineBarrier2(cmd, &preDep);

        vkCmdCopyBufferToImage(cmd, stagingBuffer, dstImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copyRegion);

        // Post-barrier: TRANSFER_DST -> SHADER_READ_ONLY. dst stage is BOTTOM_OF_PIPE (transfer-queue-compatible);
        // the cross-queue dependency to a fragment-shader read on graphics is supplied by the upload-fence wait
        // chain (DrainPendingBinds gates BindlessDescriptorSet::BindTexture on the fence), so no FRAGMENT_SHADER
        // stage mask is needed inside this transfer-queue barrier.
        VkImageMemoryBarrier2 post = pre;
        post.oldLayout     = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        post.newLayout     = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        post.srcStageMask  = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
        post.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        post.dstStageMask  = VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT;
        post.dstAccessMask = VK_ACCESS_2_NONE;
        VkDependencyInfo postDep{ VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
        postDep.imageMemoryBarrierCount = 1;
        postDep.pImageMemoryBarriers    = &post;
        vkCmdPipelineBarrier2(cmd, &postDep);

        vkEndCommandBuffer(cmd);

        VkSemaphoreSubmitInfo signalInfo{ VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO };
        signalInfo.semaphore = m_UploadTimeline.GetHandle();
        signalInfo.value     = fenceValue;
        signalInfo.stageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
        VkCommandBufferSubmitInfo cmdInfo{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO };
        cmdInfo.commandBuffer = cmd;
        VkSubmitInfo2 submitInfo{ VK_STRUCTURE_TYPE_SUBMIT_INFO_2 };
        submitInfo.commandBufferInfoCount   = 1;
        submitInfo.pCommandBufferInfos      = &cmdInfo;
        submitInfo.signalSemaphoreInfoCount = 1;
        submitInfo.pSignalSemaphoreInfos    = &signalInfo;
        VulkanContext::Get().SubmitTransfer2(submitInfo, VK_NULL_HANDLE);

        m_CurrentValue = fenceValue;
        m_InFlightBlocks.push_back({ stagingOffset, size, fenceValue });
        RecordTransferRingSlotFence(fenceValue);

        return fenceValue;
    }

    u64 UploadContext::UploadImageMipped(const void* data, u64 size, VkImage dstImage,
                                         u32 width, u32 height, u32 mipLevels, u32 arrayLayers)
    {
        std::lock_guard<std::mutex> lock(m_Lock);
        LH_PROFILE_FUNCTION();

        void* stagingPtr;
        VkBuffer stagingBuffer;
        u64 stagingOffset;

        u64 fenceValue = AllocateStaging(size, 4, &stagingPtr, stagingBuffer, stagingOffset);

        memcpy(stagingPtr, data, size);

        // Mipped path runs on the graphics queue: vkCmdBlitImage requires VK_QUEUE_GRAPHICS_BIT per spec, so it
        // can't share the transfer ring. Inner barriers stay sync1 (legacy vkCmdPipelineBarrier); they run on the
        // graphics queue where FRAGMENT_SHADER_BIT is valid, and converting the blit-chain to sync2 is out of scope.
        VkCommandBuffer cmd = BeginBlitRingSlot();

        VkCommandBufferBeginInfo beginInfo{};
        beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkBeginCommandBuffer(cmd, &beginInfo);

        // Pre-barrier: all mips UNDEFINED -> TRANSFER_DST. Mip 0 receives the staging copy;
        // mips 1..N-1 are transitioned individually back to TRANSFER_SRC during blit.
        VkImageMemoryBarrier barrier{};
        barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.image = dstImage;
        barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        barrier.subresourceRange.baseMipLevel = 0;
        barrier.subresourceRange.levelCount = mipLevels;
        barrier.subresourceRange.baseArrayLayer = 0;
        barrier.subresourceRange.layerCount = arrayLayers;
        barrier.srcAccessMask = 0;
        barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;

        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
            0, 0, nullptr, 0, nullptr, 1, &barrier);

        // Mip 0 staging copy
        VkBufferImageCopy region{};
        region.bufferOffset = stagingOffset;
        region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        region.imageSubresource.mipLevel = 0;
        region.imageSubresource.baseArrayLayer = 0;
        region.imageSubresource.layerCount = arrayLayers;
        region.imageExtent = { width, height, 1 };

        vkCmdCopyBufferToImage(cmd, stagingBuffer, dstImage,
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

        if (mipLevels > 1)
        {
            i32 mipWidth = static_cast<i32>(width);
            i32 mipHeight = static_cast<i32>(height);

            for (u32 i = 1; i < mipLevels; i++)
            {
                // Transition mip i-1: TRANSFER_DST -> TRANSFER_SRC (becomes blit source)
                barrier.subresourceRange.baseMipLevel = i - 1;
                barrier.subresourceRange.levelCount = 1;
                barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
                barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
                barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
                barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;

                vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                    0, 0, nullptr, 0, nullptr, 1, &barrier);

                VkImageBlit blit{};
                blit.srcOffsets[0] = { 0, 0, 0 };
                blit.srcOffsets[1] = { mipWidth, mipHeight, 1 };
                blit.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
                blit.srcSubresource.mipLevel = i - 1;
                blit.srcSubresource.baseArrayLayer = 0;
                blit.srcSubresource.layerCount = arrayLayers;

                i32 nextWidth = mipWidth > 1 ? mipWidth / 2 : 1;
                i32 nextHeight = mipHeight > 1 ? mipHeight / 2 : 1;

                blit.dstOffsets[0] = { 0, 0, 0 };
                blit.dstOffsets[1] = { nextWidth, nextHeight, 1 };
                blit.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
                blit.dstSubresource.mipLevel = i;
                blit.dstSubresource.baseArrayLayer = 0;
                blit.dstSubresource.layerCount = arrayLayers;

                vkCmdBlitImage(cmd,
                    dstImage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                    dstImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                    1, &blit, VK_FILTER_LINEAR);

                // Transition mip i-1: TRANSFER_SRC -> SHADER_READ_ONLY (final, won't be touched again)
                barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
                barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
                barrier.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
                barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;

                vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                    0, 0, nullptr, 0, nullptr, 1, &barrier);

                mipWidth = nextWidth;
                mipHeight = nextHeight;
            }

            // Last mip stayed in TRANSFER_DST (never blitted from); transition to SHADER_READ_ONLY.
            barrier.subresourceRange.baseMipLevel = mipLevels - 1;
            barrier.subresourceRange.levelCount = 1;
            barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;

            vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                0, 0, nullptr, 0, nullptr, 1, &barrier);
        }
        else
        {
            // Single-mip: transition the lone mip 0 to SHADER_READ_ONLY.
            barrier.subresourceRange.baseMipLevel = 0;
            barrier.subresourceRange.levelCount = 1;
            barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;

            vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                0, 0, nullptr, 0, nullptr, 1, &barrier);
        }

        vkEndCommandBuffer(cmd);

        // Submit on the graphics queue (BLIT_DST is graphics-only). Shared m_UploadTimeline: both rings signal
        // monotonically into the same timeline so DrainPendingBinds polls one value regardless of which queue.
        VkSemaphoreSubmitInfo signalInfo{ VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO };
        signalInfo.semaphore = m_UploadTimeline.GetHandle();
        signalInfo.value     = fenceValue;
        signalInfo.stageMask = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT;
        VkCommandBufferSubmitInfo cmdInfo{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO };
        cmdInfo.commandBuffer = cmd;
        VkSubmitInfo2 submitInfo{ VK_STRUCTURE_TYPE_SUBMIT_INFO_2 };
        submitInfo.commandBufferInfoCount   = 1;
        submitInfo.pCommandBufferInfos      = &cmdInfo;
        submitInfo.signalSemaphoreInfoCount = 1;
        submitInfo.pSignalSemaphoreInfos    = &signalInfo;
        VulkanContext::Get().SubmitGraphics2(submitInfo, VK_NULL_HANDLE);

        m_CurrentValue = fenceValue;
        m_InFlightBlocks.push_back({ stagingOffset, size, fenceValue });
        RecordBlitRingSlotFence(fenceValue);

        return fenceValue;
    }

    bool UploadContext::IsComplete(u64 fenceValue)
    {
        return m_UploadTimeline.GetValue() >= fenceValue;
    }

    u64 UploadContext::CompletedUploadValue()
    {
        return m_UploadTimeline.GetValue();
    }

    void UploadContext::PushPendingBind(u32* outIndex, VkImageView view, VkSampler sampler, u64 fenceValue)
    {
        std::lock_guard<std::mutex> lock(m_Lock);
        m_PendingBinds.push_back({ outIndex, view, sampler, fenceValue });
    }

    void UploadContext::DrainPendingBinds()
    {
        std::lock_guard<std::mutex> lock(m_Lock);
        LH_PROFILE_FUNCTION();

        if (m_PendingBinds.empty()) return;

        // Single GetValue call per drain: fence values are monotonic, completedValue captured
        // once is a safe lower bound for the rest of this iteration.
        const u64 completedValue = m_UploadTimeline.GetValue();
        auto& bindless = VulkanContext::Get().GetBindlessSet();

        // Reverse swap-and-pop: removing index i never invalidates indices < i.
        for (i64 i = static_cast<i64>(m_PendingBinds.size()) - 1; i >= 0; --i)
        {
            if (m_PendingBinds[i].fenceValue > completedValue) continue;

            const u32 slot = bindless.BindTexture(m_PendingBinds[i].view, m_PendingBinds[i].sampler);
            *(m_PendingBinds[i].outIndex) = slot;

            m_PendingBinds[i] = m_PendingBinds.back();
            m_PendingBinds.pop_back();
        }
    }

    void UploadContext::CancelPendingBind(VkImageView view)
    {
        std::lock_guard<std::mutex> lock(m_Lock);

        // VKTexture pushes at most one entry per ctor; linear scan finds the first (only) match.
        for (auto it = m_PendingBinds.begin(); it != m_PendingBinds.end(); ++it)
        {
            if (it->view == view)
            {
                m_PendingBinds.erase(it);
                return;
            }
        }
    }

    void UploadContext::WaitForUpload(u64 fenceValue)
    {
        m_UploadTimeline.Wait(fenceValue);
    }
}
