#include "Luthpch.h"
#include "luth/renderer/vulkan/VKDevice.h"
#include "luth/renderer/vulkan/VKCommon.h"

namespace Luth
{
    QueueFamilyIndices VKPhysicalDevice::FindQueueFamilies(VkSurfaceKHR surface) const
    {
        QueueFamilyIndices indices;

        uint32_t queueFamilyCount = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(m_PhysicalDevice, &queueFamilyCount, nullptr);
        std::vector<VkQueueFamilyProperties> queueFamilies(queueFamilyCount);
        vkGetPhysicalDeviceQueueFamilyProperties(m_PhysicalDevice, &queueFamilyCount, queueFamilies.data());

        for (uint32_t i = 0; i < queueFamilies.size(); i++) {
            // Graphics queue
            if (queueFamilies[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) {
                indices.graphicsFamily = i;
            }

            // Present queue
            VkBool32 presentSupport = false;
            vkGetPhysicalDeviceSurfaceSupportKHR(m_PhysicalDevice, i, surface, &presentSupport);
            if (presentSupport) {
                indices.presentFamily = i;
            }

            // Dedicated transfer queue (not graphics/compute)
            if ((queueFamilies[i].queueFlags & VK_QUEUE_TRANSFER_BIT) &&
                !(queueFamilies[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) &&
                !(queueFamilies[i].queueFlags & VK_QUEUE_COMPUTE_BIT)) {
                indices.transferFamily = i;
            }
        }

        // Fallback to graphics queue if no dedicated transfer
        if (!indices.transferFamily.has_value()) {
            indices.transferFamily = indices.graphicsFamily;
        }

        LH_CORE_ASSERT(indices.IsComplete(), "Failed to find suitable queue families!");
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
        bool suitable = true;

        // 1. Mandatory requirements
        // -----------------------------
        const std::vector<const char*> requiredExtensions = {
            VK_KHR_SWAPCHAIN_EXTENSION_NAME
        };

        uint32_t extensionCount;
        vkEnumerateDeviceExtensionProperties(device, nullptr, &extensionCount, nullptr);
        std::vector<VkExtensionProperties> availableExtensions(extensionCount);
        vkEnumerateDeviceExtensionProperties(device, nullptr, &extensionCount, availableExtensions.data());

        std::set<std::string> requiredSet(requiredExtensions.begin(), requiredExtensions.end());
        for (const auto& ext : availableExtensions) {
            requiredSet.erase(ext.extensionName);
        }
        if (!requiredSet.empty()) {
            LH_CORE_TRACE("Device {0} is missing required extensions", deviceProperties.deviceName);
            suitable = false;
        }

        if (!deviceFeatures.geometryShader) {
            LH_CORE_TRACE("Device {0} lacks geometry shader support", deviceProperties.deviceName);
            suitable = false;
        }

        if (!suitable) {
            m_Score = 0;
            return;
        }

        // 2. Score optional features
        // -----------------------------
        if (deviceProperties.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) {
            score += 1000;
        }

        score += deviceProperties.limits.maxImageDimension2D;

        m_Score = score;

        LH_CORE_INFO("Device {0} scored {1}", deviceProperties.deviceName, score);
    }

    VKLogicalDevice::VKLogicalDevice(VkPhysicalDevice physicalDevice,
        const QueueFamilyIndices& queueIndices,
        const std::vector<const char*>& extensions) 
        : m_QueueIndices(queueIndices)
    {
        // Queue create info
        std::vector<VkDeviceQueueCreateInfo> queueCreateInfos;
        std::set<uint32_t> uniqueQueueFamilies = {
            queueIndices.graphicsFamily.value(),
            queueIndices.presentFamily.value(),
            queueIndices.transferFamily.value()
        };

        const float queuePriority = 1.0f;
        for (uint32_t queueFamily : uniqueQueueFamilies) {
            VkDeviceQueueCreateInfo queueCreateInfo{};
            queueCreateInfo.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
            queueCreateInfo.queueFamilyIndex = queueFamily;
            queueCreateInfo.queueCount = 1;
            queueCreateInfo.pQueuePriorities = &queuePriority;
            queueCreateInfos.push_back(queueCreateInfo);
        }

        // Device features
        VkPhysicalDeviceFeatures deviceFeatures{};

        // Device create info
        VkDeviceCreateInfo createInfo{};
        createInfo.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
        createInfo.queueCreateInfoCount = static_cast<uint32_t>(queueCreateInfos.size());
        createInfo.pQueueCreateInfos = queueCreateInfos.data();
        createInfo.pEnabledFeatures = &deviceFeatures;
        createInfo.enabledExtensionCount = static_cast<uint32_t>(m_DeviceExtensions.size());
        createInfo.ppEnabledExtensionNames = m_DeviceExtensions.data();

        VK_CHECK_RESULT(vkCreateDevice(physicalDevice, &createInfo, nullptr, &m_Device),
            "Failed to create logical device!");

        vkGetDeviceQueue(m_Device, queueIndices.graphicsFamily.value(), 0, &m_GraphicsQueue);
        vkGetDeviceQueue(m_Device, queueIndices.presentFamily.value(), 0, &m_PresentQueue);
        vkGetDeviceQueue(m_Device, queueIndices.transferFamily.value(), 0, &m_TransferQueue);
    }

    VKLogicalDevice::~VKLogicalDevice()
    {
        if (m_Device) {
            vkDestroyDevice(m_Device, nullptr);
        }
    }
}
