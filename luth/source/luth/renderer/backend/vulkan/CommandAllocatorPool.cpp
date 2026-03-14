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
        SpinLockGuard lock(m_Lock);
        
        for (CommandAllocator* allocator : m_AllAllocators)
        {
            vkDestroyCommandPool(m_Device, allocator->GetPool(), nullptr);
            delete allocator;
        }
        
        m_AllAllocators.clear();
        m_AvailableAllocators.clear();
    }

    CommandAllocator* CommandAllocatorPool::Acquire()
    {
        SpinLockGuard lock(m_Lock);

        if (!m_AvailableAllocators.empty())
        {
            CommandAllocator* allocator = m_AvailableAllocators.back();
            m_AvailableAllocators.pop_back();
            return allocator;
        }

        return CreateAllocator();
    }

    void CommandAllocatorPool::Release(CommandAllocator* allocator)
    {
        SpinLockGuard lock(m_Lock);
        m_AvailableAllocators.push_back(allocator);
    }

    void CommandAllocatorPool::ResetAll()
    {
        SpinLockGuard lock(m_Lock);
        
        for (CommandAllocator* allocator : m_AllAllocators)
        {
            allocator->Reset();
        }
        
        // Reset availability just in case (though logic should ensure they are returned)
        m_AvailableAllocators = m_AllAllocators;
    }

    CommandAllocator* CommandAllocatorPool::CreateAllocator()
    {
        LH_CORE_WARN("Creating new Command Allocator. Total: {0}", m_AllAllocators.size() + 1);

        VkCommandPoolCreateInfo poolInfo{};
        poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        poolInfo.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT; // Hint that buffers are short-lived
        poolInfo.queueFamilyIndex = m_QueueFamilyIndex;

        VkCommandPool pool;
        if (vkCreateCommandPool(m_Device, &poolInfo, nullptr, &pool) != VK_SUCCESS)
        {
            LH_CORE_CRITICAL("Failed to create Command Pool!");
            return nullptr;
        }

        CommandAllocator* allocator = new CommandAllocator(m_Device, pool);
        m_AllAllocators.push_back(allocator);
        return allocator;
    }
}
