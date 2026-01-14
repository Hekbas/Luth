#include "luthpch.h"
#include "CommandAllocator.h"
#include "luth/core/Log.h"

namespace Luth
{
    CommandAllocator::CommandAllocator(VkDevice device, VkCommandPool pool)
        : m_Device(device), m_Pool(pool)
    {
    }

    CommandAllocator::~CommandAllocator()
    {
        // We don't destroy the pool here because it's owned by CommandAllocatorPool.
        // We just clear our cache.
        // Actually, if we allocated buffers, we should free them?
        // No, destroying the pool frees the buffers.
        // But since we don't own the pool, we assume the pool owner handles destruction.
    }

    VkCommandBuffer CommandAllocator::GetBuffer()
    {
        // 1. Check cache
        if (m_UsedCount < m_Buffers.size())
        {
            return m_Buffers[m_UsedCount++];
        }

        // 2. Allocate new
        VkCommandBufferAllocateInfo allocInfo{};
        allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        allocInfo.commandPool = m_Pool;
        allocInfo.level = VK_COMMAND_BUFFER_LEVEL_SECONDARY; // Default to Secondary for parallel recording
        allocInfo.commandBufferCount = 1;

        VkCommandBuffer cmd;
        if (vkAllocateCommandBuffers(m_Device, &allocInfo, &cmd) != VK_SUCCESS)
        {
            LH_CORE_CRITICAL("Failed to allocate command buffer!");
            return VK_NULL_HANDLE;
        }

        m_Buffers.push_back(cmd);
        m_UsedCount++;
        return cmd;
    }

    void CommandAllocator::Reset()
    {
        // Resetting the pool recycles all buffers at once.
        // We just reset our index.
        vkResetCommandPool(m_Device, m_Pool, 0);
        m_UsedCount = 0;
    }
}
