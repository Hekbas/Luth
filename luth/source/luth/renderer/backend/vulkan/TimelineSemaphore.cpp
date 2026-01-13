#include "luthpch.h"
#include "TimelineSemaphore.h"
#include "VulkanContext.h"
#include "luth/core/Log.h"

namespace Luth
{
    TimelineSemaphore::TimelineSemaphore(u64 initialValue)
    {
        Init(initialValue);
    }

    TimelineSemaphore::~TimelineSemaphore()
    {
        Shutdown();
    }

    void TimelineSemaphore::Init(u64 initialValue)
    {
        // Ensure VulkanContext is initialized before creating semaphore
        // This check is useful if Init is called manually
        // But the crash happened because the constructor called Init() before Context was ready.

        VkSemaphoreTypeCreateInfo typeCreateInfo{};
        typeCreateInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO;
        typeCreateInfo.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
        typeCreateInfo.initialValue = initialValue;

        VkSemaphoreCreateInfo createInfo{};
        createInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
        createInfo.pNext = &typeCreateInfo;

        if (vkCreateSemaphore(VulkanContext::Get().GetDevice(), &createInfo, nullptr, &m_Semaphore) != VK_SUCCESS)
        {
            LH_CORE_CRITICAL("Failed to create Timeline Semaphore!");
        }
    }

    void TimelineSemaphore::Shutdown()
    {
        if (m_Semaphore != VK_NULL_HANDLE)
        {
            vkDestroySemaphore(VulkanContext::Get().GetDevice(), m_Semaphore, nullptr);
            m_Semaphore = VK_NULL_HANDLE;
        }
    }

    void TimelineSemaphore::Signal(u64 value)
    {
        VkSemaphoreSignalInfo signalInfo{};
        signalInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SIGNAL_INFO;
        signalInfo.semaphore = m_Semaphore;
        signalInfo.value = value;

        if (vkSignalSemaphore(VulkanContext::Get().GetDevice(), &signalInfo) != VK_SUCCESS)
        {
            LH_CORE_ERROR("Failed to signal Timeline Semaphore!");
        }
    }

    bool TimelineSemaphore::Wait(u64 value, u64 timeoutNanoseconds)
    {
        VkSemaphoreWaitInfo waitInfo{};
        waitInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO;
        waitInfo.semaphoreCount = 1;
        waitInfo.pSemaphores = &m_Semaphore;
        waitInfo.pValues = &value;

        VkResult result = vkWaitSemaphores(VulkanContext::Get().GetDevice(), &waitInfo, timeoutNanoseconds);
        return result == VK_SUCCESS;
    }

    u64 TimelineSemaphore::GetValue() const
    {
        u64 value = 0;
        if (vkGetSemaphoreCounterValue(VulkanContext::Get().GetDevice(), m_Semaphore, &value) != VK_SUCCESS)
        {
            LH_CORE_ERROR("Failed to get Timeline Semaphore value!");
        }
        return value;
    }
}
