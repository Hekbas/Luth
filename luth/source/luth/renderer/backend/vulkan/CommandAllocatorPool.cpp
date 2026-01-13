#include "luthpch.h"
#include "CommandAllocatorPool.h"
#include "VulkanContext.h"
#include "luth/core/Log.h"

namespace Luth
{
    CommandAllocatorPool::CommandAllocatorPool(u32 queueFamilyIndex)
        : m_QueueFamilyIndex(queueFamilyIndex)
    {
    }

    CommandAllocatorPool::~CommandAllocatorPool()
    {
        Shutdown();
    }

    void CommandAllocatorPool::Init()
    {
        m_Device = VulkanContext::Get().GetDevice();
    }

    void CommandAllocatorPool::Shutdown()
    {
        std::lock_guard<std::mutex> lock(m_Lock);
        
        for (VkCommandPool pool : m_AllPools)
        {
            vkDestroyCommandPool(m_Device, pool, nullptr);
        }
        
        m_AllPools.clear();
        m_AvailablePools.clear();
    }

    VkCommandPool CommandAllocatorPool::AcquirePool()
    {
        std::lock_guard<std::mutex> lock(m_Lock);

        if (!m_AvailablePools.empty())
        {
            VkCommandPool pool = m_AvailablePools.back();
            m_AvailablePools.pop_back();
            return pool;
        }

        return CreatePool();
    }

    void CommandAllocatorPool::ReleasePool(VkCommandPool pool)
    {
        std::lock_guard<std::mutex> lock(m_Lock);
        m_AvailablePools.push_back(pool);
    }

    void CommandAllocatorPool::ResetAll()
    {
        std::lock_guard<std::mutex> lock(m_Lock);
        
        for (VkCommandPool pool : m_AllPools)
        {
            vkResetCommandPool(m_Device, pool, 0);
        }
        
        // In a more complex system, we might want to ensure all pools are returned before resetting.
        // For now, we assume this is called at a safe point (start of frame) where no recording is happening.
    }

    VkCommandPool CommandAllocatorPool::CreatePool()
    {
        VkCommandPoolCreateInfo poolInfo{};
        poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        poolInfo.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT; // Hint that buffers are short-lived
        poolInfo.queueFamilyIndex = m_QueueFamilyIndex;

        VkCommandPool pool;
        if (vkCreateCommandPool(m_Device, &poolInfo, nullptr, &pool) != VK_SUCCESS)
        {
            LH_CORE_CRITICAL("Failed to create Command Pool!");
        }

        m_AllPools.push_back(pool);
        return pool;
    }
}
