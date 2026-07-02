#include "luthpch.h"
#include "CommandAllocator.h"
#include "luth/core/diagnostics/Log.h"

namespace Luth
{
    CommandAllocator::CommandAllocator(VkDevice device, VkCommandPool pool)
        : m_Device(device), m_Pool(pool)
    {
    }

    CommandAllocator::~CommandAllocator()
    {
        // Pool ownership lives in CommandAllocatorPool; destroying it frees all buffers allocated from it,
        // so this destructor has nothing to free explicitly.
    }

    VkCommandBuffer CommandAllocator::GetBuffer(VkCommandBufferLevel level)
    {
        // Reuse an already-allocated (but reset) command buffer if available. After vkResetCommandPool all
        // existing buffers are in the initial state and can be re-recorded with vkBeginCommandBuffer.
        if (m_UsedCount < (u32)m_Buffers.size())
        {
            return m_Buffers[m_UsedCount++];
        }

        // No cached buffer available; allocate a new one from the pool.
        VkCommandBufferAllocateInfo allocInfo{};
        allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        allocInfo.commandPool = m_Pool;
        allocInfo.level = level;
        allocInfo.commandBufferCount = 1;

        VkCommandBuffer cmd;
        if (vkAllocateCommandBuffers(m_Device, &allocInfo, &cmd) != VK_SUCCESS)
        {
            LH_LOG(Renderer, critical, "Failed to allocate command buffer!");
            return VK_NULL_HANDLE;
        }

        m_Buffers.push_back(cmd);
        m_UsedCount++;
        return cmd;
    }

    void CommandAllocator::Reset()
    {
        // Resetting the pool recycles all buffers at once (they return to the initial state). Keep the handles
        // so GetBuffer() can reuse them next frame instead of allocating new ones every time.
        vkResetCommandPool(m_Device, m_Pool, 0);
        m_UsedCount = 0;
        // NOTE: Do NOT clear m_Buffers; the handles are still valid after pool reset.
    }
}
