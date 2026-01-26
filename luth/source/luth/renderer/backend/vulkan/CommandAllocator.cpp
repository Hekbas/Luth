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

    VkCommandBuffer CommandAllocator::GetBuffer(VkCommandBufferLevel level)
    {
        // 1. Check cache
        // Note: We currently don't segregate cache by level.
        // If we mix primary/secondary, we need to track them separately.
        // For now, let's assume we always allocate new if the cache is empty or if we want to be safe.
        // Optimization: Just allocate new for now to avoid complexity.
        // The pool reset handles recycling.
        
        // Actually, we can't reuse buffers from the vector if they were allocated with a different level.
        // Let's just always allocate for now. The pool handles the memory.
        
        VkCommandBufferAllocateInfo allocInfo{};
        allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        allocInfo.commandPool = m_Pool;
        allocInfo.level = level;
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
        m_Buffers.clear(); // Since we are re-allocating every time, clear the vector.
    }
}
