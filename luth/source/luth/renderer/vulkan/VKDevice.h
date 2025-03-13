#pragma once

#include <vulkan/vulkan.h>

namespace Luth
{
    struct QueueFamilyIndices
    {
        std::optional<uint32_t> graphicsFamily;
        std::optional<uint32_t> presentFamily;
        std::optional<uint32_t> transferFamily;

        bool IsComplete() const {
            return graphicsFamily.has_value();
        }
    };

    class VKPhysicalDevice
    {
    public:
        VKPhysicalDevice(VkInstance instance);

        bool IsSuitable() const;
        QueueFamilyIndices FindQueueFamilies(VkSurfaceKHR surface) const;
        VkPhysicalDevice GetHandle() const { return m_PhysicalDevice; }

    private:
        void RateDeviceSuitability(VkPhysicalDevice device);

        VkPhysicalDevice m_PhysicalDevice = VK_NULL_HANDLE;
        int m_Score = 0;
    };

    class VKLogicalDevice
    {
    public:
        VKLogicalDevice(VkPhysicalDevice physicalDevice,
            const QueueFamilyIndices& queueIndices,
            const std::vector<const char*>& extensions);
        ~VKLogicalDevice();

        VkDevice GetHandle() const { return m_Device; }
        VkQueue GetGraphicsQueue() const { return m_GraphicsQueue; }
        VkQueue GetPresentQueue() const { return m_PresentQueue; }
        VkQueue GetTransferQueue() const { return m_TransferQueue; }
        const QueueFamilyIndices& GetQueueFamilyIndices() const { return m_QueueIndices; }

    private:
        VkDevice m_Device = VK_NULL_HANDLE;
        VkQueue m_GraphicsQueue = VK_NULL_HANDLE;
        VkQueue m_PresentQueue = VK_NULL_HANDLE;
        VkQueue m_TransferQueue = VK_NULL_HANDLE;
        QueueFamilyIndices m_QueueIndices;

        const std::vector<const char*> m_DeviceExtensions = {
            VK_KHR_SWAPCHAIN_EXTENSION_NAME
        };
    };
}
