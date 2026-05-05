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
        LH_CORE_INFO("Upload Context Initialized (Staging: 64MB)");
    }

    void UploadContext::Shutdown()
    {
        if (!s_Instance) return;

        VkDevice device = VulkanContext::Get().GetDevice();
        
        // Wait for all uploads
        s_Instance->m_UploadTimeline.Wait(s_Instance->m_CurrentValue);

        s_Instance->m_UploadTimeline.Shutdown();
        vkDestroyCommandPool(device, s_Instance->m_CommandPool, nullptr);
        
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
        VkDevice device = VulkanContext::Get().GetDevice();
        
        // 1. Get Transfer Queue
        m_TransferQueue = VulkanContext::Get().GetGraphicsQueue(); 
        u32 queueFamily = VulkanContext::Get().GetGraphicsFamily();

        // 2. Command Pool
        VkCommandPoolCreateInfo poolInfo{};
        poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        poolInfo.queueFamilyIndex = queueFamily;
        poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        vkCreateCommandPool(device, &poolInfo, nullptr, &m_CommandPool);

        // 3. Command Buffer Ring (RING_SIZE buffers reused round-robin, gated by per-slot fence values)
        VkCommandBufferAllocateInfo allocInfo{};
        allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        allocInfo.commandPool = m_CommandPool;
        allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        allocInfo.commandBufferCount = RING_SIZE;
        vkAllocateCommandBuffers(device, &allocInfo, m_CmdRing.data());

        // 4. Timeline Semaphore
        m_UploadTimeline.Init(0);

        // 5. Staging Buffer
        VkBufferCreateInfo bufferInfo = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
        bufferInfo.size = STAGING_SIZE;
        bufferInfo.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
        
        m_StagingAllocation = VulkanAllocator::AllocateBuffer(bufferInfo, VMA_MEMORY_USAGE_CPU_ONLY, m_StagingBuffer);
        m_StagingMapped = VulkanAllocator::Map(m_StagingAllocation);
    }

    VkCommandBuffer UploadContext::BeginRingSlot()
    {
        const u32 slot = m_SubmitIndex % RING_SIZE;
        const u64 oldFence = m_RingFenceValues[slot];

        // Wait only if this slot's previous submission is still in flight — submits on different
        // slots overlap on the GPU. The staging ring typically saturates first in practice.
        if (oldFence > 0 && m_UploadTimeline.GetValue() < oldFence)
            m_UploadTimeline.Wait(oldFence);

        VkCommandBuffer cmd = m_CmdRing[slot];
        vkResetCommandBuffer(cmd, 0);
        return cmd;
    }

    void UploadContext::RecordRingSlotFence(u64 fenceValue)
    {
        const u32 slot = m_SubmitIndex % RING_SIZE;
        m_RingFenceValues[slot] = fenceValue;
        m_SubmitIndex++;
    }

    u64 UploadContext::AllocateStaging(u64 size, u64 alignment, void** outMappedPtr, VkBuffer& outBuffer, u64& outOffset)
    {
        LH_CORE_ASSERT(size <= STAGING_SIZE, "AllocateStaging: size exceeds ring capacity");

        u64 alignedHead = (m_StagingHead + (alignment - 1)) & ~(alignment - 1);

        // invariant: after wrap, the ring is in "wrapped" state for *this* call —
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

        VkCommandBuffer cmd = BeginRingSlot();

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

        VkSubmitInfo submitInfo{};
        submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submitInfo.commandBufferCount = 1;
        submitInfo.pCommandBuffers = &cmd;

        VkTimelineSemaphoreSubmitInfo timelineInfo{};
        timelineInfo.sType = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO;
        timelineInfo.signalSemaphoreValueCount = 1;
        timelineInfo.pSignalSemaphoreValues = &fenceValue;

        VkSemaphore signalSemaphore = m_UploadTimeline.GetHandle();
        submitInfo.pSignalSemaphores = &signalSemaphore;
        submitInfo.signalSemaphoreCount = 1;
        submitInfo.pNext = &timelineInfo;

        vkQueueSubmit(m_TransferQueue, 1, &submitInfo, VK_NULL_HANDLE);

        m_CurrentValue = fenceValue;
        m_InFlightBlocks.push_back({ stagingOffset, size, fenceValue });
        RecordRingSlotFence(fenceValue);

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

        // Adjust copy region buffer offset
        copyRegion.bufferOffset += stagingOffset;

        VkCommandBuffer cmd = BeginRingSlot();

        VkCommandBufferBeginInfo beginInfo{};
        beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkBeginCommandBuffer(cmd, &beginInfo);

        // Transition to Transfer Dst
        VkImageMemoryBarrier barrier{};
        barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.image = dstImage;

        // Manually copy subresource fields because VkImageSubresourceLayers != VkImageSubresourceRange
        barrier.subresourceRange.aspectMask = copyRegion.imageSubresource.aspectMask;
        barrier.subresourceRange.baseMipLevel = copyRegion.imageSubresource.mipLevel;
        barrier.subresourceRange.levelCount = 1;
        barrier.subresourceRange.baseArrayLayer = copyRegion.imageSubresource.baseArrayLayer;
        barrier.subresourceRange.layerCount = copyRegion.imageSubresource.layerCount;

        barrier.srcAccessMask = 0;
        barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;

        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);

        vkCmdCopyBufferToImage(cmd, stagingBuffer, dstImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copyRegion);

        // Transition to Shader Read
        barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;

        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);

        vkEndCommandBuffer(cmd);

        VkSubmitInfo submitInfo{};
        submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submitInfo.commandBufferCount = 1;
        submitInfo.pCommandBuffers = &cmd;

        VkTimelineSemaphoreSubmitInfo timelineInfo{};
        timelineInfo.sType = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO;
        timelineInfo.signalSemaphoreValueCount = 1;
        timelineInfo.pSignalSemaphoreValues = &fenceValue;

        VkSemaphore signalSemaphore = m_UploadTimeline.GetHandle();
        submitInfo.pSignalSemaphores = &signalSemaphore;
        submitInfo.signalSemaphoreCount = 1;
        submitInfo.pNext = &timelineInfo;

        vkQueueSubmit(m_TransferQueue, 1, &submitInfo, VK_NULL_HANDLE);

        m_CurrentValue = fenceValue;
        m_InFlightBlocks.push_back({ stagingOffset, size, fenceValue });
        RecordRingSlotFence(fenceValue);

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

        VkCommandBuffer cmd = BeginRingSlot();

        VkCommandBufferBeginInfo beginInfo{};
        beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkBeginCommandBuffer(cmd, &beginInfo);

        // Pre-barrier: all mips UNDEFINED → TRANSFER_DST. Mip 0 receives the staging copy;
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
                // Transition mip i-1: TRANSFER_DST → TRANSFER_SRC (becomes blit source)
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

                // Transition mip i-1: TRANSFER_SRC → SHADER_READ_ONLY (final, won't be touched again)
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

        VkSubmitInfo submitInfo{};
        submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submitInfo.commandBufferCount = 1;
        submitInfo.pCommandBuffers = &cmd;

        VkTimelineSemaphoreSubmitInfo timelineInfo{};
        timelineInfo.sType = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO;
        timelineInfo.signalSemaphoreValueCount = 1;
        timelineInfo.pSignalSemaphoreValues = &fenceValue;

        VkSemaphore signalSemaphore = m_UploadTimeline.GetHandle();
        submitInfo.pSignalSemaphores = &signalSemaphore;
        submitInfo.signalSemaphoreCount = 1;
        submitInfo.pNext = &timelineInfo;

        vkQueueSubmit(m_TransferQueue, 1, &submitInfo, VK_NULL_HANDLE);

        m_CurrentValue = fenceValue;
        m_InFlightBlocks.push_back({ stagingOffset, size, fenceValue });
        RecordRingSlotFence(fenceValue);

        return fenceValue;
    }

    bool UploadContext::IsComplete(u64 fenceValue)
    {
        return m_UploadTimeline.GetValue() >= fenceValue;
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

        // Single GetValue call per drain — fence values are monotonic, completedValue captured
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
