#pragma once

namespace Luth
{
    struct QueueFamilyIndices
    {
        std::optional<uint32_t> graphicsFamily;

        bool IsComplete() const {
            return graphicsFamily.has_value();
        }
    };

    class VKPhysicalDevice
    {
    public:
        VKPhysicalDevice(VkInstance instance);

        bool IsSuitable() const;
        QueueFamilyIndices FindQueueFamilies() const;
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
            const QueueFamilyIndices& queueIndices);
        ~VKLogicalDevice();

        VkDevice GetHandle() const { return m_Device; }
        VkQueue GetGraphicsQueue() const { return m_GraphicsQueue; }

    private:
        VkDevice m_Device = VK_NULL_HANDLE;
        VkQueue m_GraphicsQueue = VK_NULL_HANDLE;
    };
}
