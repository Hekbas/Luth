#include "Luthpch.h"
#include "luth/renderer/vulkan/VKDevice.h"
#include "luth/renderer/vulkan/VKCommon.h"

namespace Luth
{
    QueueFamilyIndices VKPhysicalDevice::FindQueueFamilies() const
    {
        QueueFamilyIndices indices;

        uint32_t queueFamilyCount = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(m_PhysicalDevice, &queueFamilyCount, nullptr);
        std::vector<VkQueueFamilyProperties> queueFamilies(queueFamilyCount);
        vkGetPhysicalDeviceQueueFamilyProperties(m_PhysicalDevice, &queueFamilyCount, queueFamilies.data());

        int i = 0;
        for (const auto& queueFamily : queueFamilies) {
            if (queueFamily.queueFlags & VK_QUEUE_GRAPHICS_BIT) {
                indices.graphicsFamily = i;
            }
            if (indices.IsComplete()) break;
            i++;
        }

        return indices;
    }

    VKPhysicalDevice::VKPhysicalDevice(VkInstance instance)
    {
        uint32_t deviceCount = 0;
        vkEnumeratePhysicalDevices(instance, &deviceCount, nullptr);

        if (deviceCount == 0) {
            LH_CORE_ASSERT(false, "Failed to find GPUs with Vulkan support!");
        }

        std::vector<VkPhysicalDevice> devices(deviceCount);
        vkEnumeratePhysicalDevices(instance, &deviceCount, devices.data());

        // Select best device
        for (const auto& device : devices) {
            RateDeviceSuitability(device);
            if (m_Score > 0) {
                m_PhysicalDevice = device;
                break;
            }
        }

        if (m_PhysicalDevice == VK_NULL_HANDLE) {
            LH_CORE_ASSERT(false, "Failed to find a suitable GPU!");
        }

        VkPhysicalDeviceProperties deviceProperties;
        vkGetPhysicalDeviceProperties(m_PhysicalDevice, &deviceProperties);
        LH_CORE_INFO("Selected Vulkan device: {0}", deviceProperties.deviceName);
    }

    bool VKPhysicalDevice::IsSuitable() const
    {
        return m_Score > 0;
    }

    void VKPhysicalDevice::RateDeviceSuitability(VkPhysicalDevice device)
    {
        VkPhysicalDeviceProperties deviceProperties;
        VkPhysicalDeviceFeatures deviceFeatures;
        vkGetPhysicalDeviceProperties(device, &deviceProperties);
        vkGetPhysicalDeviceFeatures(device, &deviceFeatures);

        int score = 0;

        // Discrete GPUs have significant performance advantage
        if (deviceProperties.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) {
            score += 1000;
        }

        // Maximum possible size of textures affects graphics quality
        score += deviceProperties.limits.maxImageDimension2D;

        // Application can't function without geometry shaders
        if (!deviceFeatures.geometryShader) {
            return;
        }

        m_Score = score;
    }

    VKLogicalDevice::VKLogicalDevice(VkPhysicalDevice physicalDevice, const QueueFamilyIndices& queueIndices)
    {
        // Queue create info
        std::vector<VkDeviceQueueCreateInfo> queueCreateInfos;
        const float queuePriority = 1.0f;

        VkDeviceQueueCreateInfo queueCreateInfo{};
        queueCreateInfo.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
        queueCreateInfo.queueFamilyIndex = queueIndices.graphicsFamily.value();
        queueCreateInfo.queueCount = 1;
        queueCreateInfo.pQueuePriorities = &queuePriority;
        queueCreateInfos.push_back(queueCreateInfo);

        // Device features
        VkPhysicalDeviceFeatures deviceFeatures{};

        // Device create info
        VkDeviceCreateInfo createInfo{};
        createInfo.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
        createInfo.queueCreateInfoCount = static_cast<uint32_t>(queueCreateInfos.size());
        createInfo.pQueueCreateInfos = queueCreateInfos.data();
        createInfo.pEnabledFeatures = &deviceFeatures;
        createInfo.enabledExtensionCount = 0;

        VK_CHECK_RESULT(vkCreateDevice(physicalDevice, &createInfo, nullptr, &m_Device),
            "Failed to create logical device!");

        vkGetDeviceQueue(m_Device, queueIndices.graphicsFamily.value(), 0, &m_GraphicsQueue);
    }

    VKLogicalDevice::~VKLogicalDevice() {
        if (m_Device) {
            vkDestroyDevice(m_Device, nullptr);
        }
    }
}
