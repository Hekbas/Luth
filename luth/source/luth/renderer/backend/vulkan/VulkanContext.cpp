#include "luthpch.h"
#include "luth/renderer/backend/vulkan/VulkanContext.h"
#include "luth/renderer/backend/vulkan/VulkanAllocator.h"
#include "luth/core/Log.h"

#include <GLFW/glfw3.h>

namespace Luth
{
    static VulkanContext* s_Instance = nullptr;

    static VKAPI_ATTR VkBool32 VKAPI_CALL DebugCallback(
        VkDebugUtilsMessageSeverityFlagBitsEXT messageSeverity,
        VkDebugUtilsMessageTypeFlagsEXT messageType,
        const VkDebugUtilsMessengerCallbackDataEXT* pCallbackData,
        void* pUserData) 
    {
        if (messageSeverity >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT)
            LH_CORE_ERROR("Validation Layer: {0}", pCallbackData->pMessage);
        else if (messageSeverity >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT)
            LH_CORE_WARN("Validation Layer: {0}", pCallbackData->pMessage);
        
        return VK_FALSE;
    }

    void VulkanContext::Init(void* windowHandle)
    {
        LH_CORE_ASSERT(!s_Instance, "VulkanContext already initialized!");
        s_Instance = new VulkanContext();
        s_Instance->m_WindowHandle = windowHandle;
        
        s_Instance->CreateInstance();
        s_Instance->SetupDebugMessenger();
        s_Instance->PickPhysicalDevice();
        s_Instance->CreateLogicalDevice();
        s_Instance->InitAllocator();
        s_Instance->m_BindlessSet.Init(s_Instance->m_Device);
        s_Instance->m_DescriptorAllocator.Init(s_Instance->m_Device);
        s_Instance->m_ResourceCache.Init();
    }

    void VulkanContext::Shutdown()
    {
        if (!s_Instance) return;

        // Flush all deletion queues
        for (int i = 0; i < MAX_FRAMES_IN_FLIGHT; i++) {
            for (auto& func : s_Instance->m_DeletionQueues[i].deletors) {
                func();
            }
            s_Instance->m_DeletionQueues[i].deletors.clear();
        }

        s_Instance->m_ResourceCache.Shutdown();
        s_Instance->m_BindlessSet.Shutdown();
        s_Instance->m_DescriptorAllocator.Shutdown();
        VulkanAllocator::Shutdown();
        vkDestroyCommandPool(s_Instance->m_Device, s_Instance->m_CommandPool, nullptr);
        vkDestroyDevice(s_Instance->m_Device, nullptr);

        if (s_Instance->m_EnableValidationLayers) {
            auto func = (PFN_vkDestroyDebugUtilsMessengerEXT)vkGetInstanceProcAddr(s_Instance->m_Instance, "vkDestroyDebugUtilsMessengerEXT");
            if (func) func(s_Instance->m_Instance, s_Instance->m_DebugMessenger, nullptr);
        }

        vkDestroyInstance(s_Instance->m_Instance, nullptr);
        
        delete s_Instance;
        s_Instance = nullptr;
    }

    VulkanContext& VulkanContext::Get()
    {
        LH_CORE_ASSERT(s_Instance, "VulkanContext not initialized!");
        return *s_Instance;
    }

    void VulkanContext::CreateInstance()
    {
        if (m_EnableValidationLayers && !CheckValidationLayerSupport()) {
            LH_CORE_ERROR("Validation layers requested, but not available!");
            m_EnableValidationLayers = false;
        }

        VkApplicationInfo appInfo{};
        appInfo.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
        appInfo.pApplicationName = "Luth Engine";
        appInfo.applicationVersion = VK_MAKE_VERSION(1, 0, 0);
        appInfo.pEngineName = "Luth";
        appInfo.engineVersion = VK_MAKE_VERSION(1, 0, 0);
        appInfo.apiVersion = VK_API_VERSION_1_3; // Target Vulkan 1.3

        VkInstanceCreateInfo createInfo{};
        createInfo.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
        createInfo.pApplicationInfo = &appInfo;

        // Extensions
        uint32_t glfwExtensionCount = 0;
        const char** glfwExtensions = glfwGetRequiredInstanceExtensions(&glfwExtensionCount);
        std::vector<const char*> extensions(glfwExtensions, glfwExtensions + glfwExtensionCount);

        if (m_EnableValidationLayers) {
            extensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
        }

        createInfo.enabledExtensionCount = static_cast<uint32_t>(extensions.size());
        createInfo.ppEnabledExtensionNames = extensions.data();

        // Layers
        if (m_EnableValidationLayers) {
            createInfo.enabledLayerCount = static_cast<uint32_t>(m_ValidationLayers.size());
            createInfo.ppEnabledLayerNames = m_ValidationLayers.data();
        } else {
            createInfo.enabledLayerCount = 0;
        }

        if (vkCreateInstance(&createInfo, nullptr, &m_Instance) != VK_SUCCESS) {
            LH_CORE_CRITICAL("Failed to create Vulkan instance!");
        }
    }

    void VulkanContext::SetupDebugMessenger()
    {
        if (!m_EnableValidationLayers) return;

        VkDebugUtilsMessengerCreateInfoEXT createInfo{};
        createInfo.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
        createInfo.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_VERBOSE_BIT_EXT | 
                                     VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | 
                                     VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
        createInfo.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT | 
                                 VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT | 
                                 VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
        createInfo.pfnUserCallback = DebugCallback;

        auto func = (PFN_vkCreateDebugUtilsMessengerEXT)vkGetInstanceProcAddr(m_Instance, "vkCreateDebugUtilsMessengerEXT");
        if (func) {
            func(m_Instance, &createInfo, nullptr, &m_DebugMessenger);
        }
    }

    void VulkanContext::PickPhysicalDevice()
    {
        uint32_t deviceCount = 0;
        vkEnumeratePhysicalDevices(m_Instance, &deviceCount, nullptr);
        if (deviceCount == 0) LH_CORE_CRITICAL("Failed to find GPUs with Vulkan support!");

        std::vector<VkPhysicalDevice> devices(deviceCount);
        vkEnumeratePhysicalDevices(m_Instance, &deviceCount, devices.data());

        // Simple selection: Pick the first discrete GPU, fallback to first available
        for (const auto& device : devices) {
            VkPhysicalDeviceProperties props;
            vkGetPhysicalDeviceProperties(device, &props);
            
            if (props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) {
                m_PhysicalDevice = device;
                m_PhysicalDeviceProperties = props;
                LH_CORE_INFO("Vulkan GPU: {0}", props.deviceName);
                break;
            }
        }

        if (m_PhysicalDevice == VK_NULL_HANDLE) {
            m_PhysicalDevice = devices[0];
            VkPhysicalDeviceProperties props;
            vkGetPhysicalDeviceProperties(m_PhysicalDevice, &props);
            m_PhysicalDeviceProperties = props;
            LH_CORE_WARN("Vulkan GPU (Integrated): {0}", props.deviceName);
        }
    }

    void VulkanContext::CreateLogicalDevice()
    {
        // Find Queue Families
        uint32_t queueFamilyCount = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(m_PhysicalDevice, &queueFamilyCount, nullptr);
        std::vector<VkQueueFamilyProperties> queueFamilies(queueFamilyCount);
        vkGetPhysicalDeviceQueueFamilyProperties(m_PhysicalDevice, &queueFamilyCount, queueFamilies.data());

        int i = 0;
        for (const auto& queueFamily : queueFamilies) {
            if (queueFamily.queueFlags & VK_QUEUE_GRAPHICS_BIT) {
                m_GraphicsFamily = i;
                break;
            }
            i++;
        }

        if (m_GraphicsFamily == -1) LH_CORE_CRITICAL("Failed to find Graphics Queue Family!");

        float queuePriority = 1.0f;
        VkDeviceQueueCreateInfo queueCreateInfo{};
        queueCreateInfo.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
        queueCreateInfo.queueFamilyIndex = m_GraphicsFamily;
        queueCreateInfo.queueCount = 1;
        queueCreateInfo.pQueuePriorities = &queuePriority;

        // Features
        VkPhysicalDeviceFeatures deviceFeatures{};
        deviceFeatures.samplerAnisotropy = VK_TRUE;

        // Vulkan 1.2 Features (Descriptor Indexing for Bindless)
        VkPhysicalDeviceVulkan12Features features12{};
        features12.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
        features12.descriptorBindingPartiallyBound = VK_TRUE;
        features12.descriptorBindingSampledImageUpdateAfterBind = VK_TRUE;
        features12.runtimeDescriptorArray = VK_TRUE;
        features12.shaderSampledImageArrayNonUniformIndexing = VK_TRUE;
        
        // Enable Timeline Semaphores (Vulkan 1.2 feature)
        features12.timelineSemaphore = VK_TRUE;

        // Vulkan 1.3 Features (Dynamic Rendering, Synchronization2)
        VkPhysicalDeviceVulkan13Features features13{};
        features13.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
        features13.dynamicRendering = VK_TRUE;
        features13.synchronization2 = VK_TRUE;
        features13.pNext = &features12;

        VkDeviceCreateInfo createInfo{};
        createInfo.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
        createInfo.pNext = &features13; // Chain 1.3 features
        createInfo.pQueueCreateInfos = &queueCreateInfo;
        createInfo.queueCreateInfoCount = 1;
        createInfo.pEnabledFeatures = &deviceFeatures;

        std::vector<const char*> deviceExtensions = {
            VK_KHR_SWAPCHAIN_EXTENSION_NAME,
            VK_KHR_DYNAMIC_RENDERING_EXTENSION_NAME
        };

        createInfo.enabledExtensionCount = static_cast<uint32_t>(deviceExtensions.size());
        createInfo.ppEnabledExtensionNames = deviceExtensions.data();

        if (vkCreateDevice(m_PhysicalDevice, &createInfo, nullptr, &m_Device) != VK_SUCCESS) {
            LH_CORE_CRITICAL("Failed to create logical device!");
        }

        vkGetDeviceQueue(m_Device, m_GraphicsFamily, 0, &m_GraphicsQueue);

        // Create Command Pool for Immediate Submits
        VkCommandPoolCreateInfo poolInfo{};
        poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        poolInfo.queueFamilyIndex = m_GraphicsFamily;
        poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        
        vkCreateCommandPool(m_Device, &poolInfo, nullptr, &m_CommandPool);
    }

    void VulkanContext::InitAllocator()
    {
        VulkanAllocator::Init(m_Instance, m_PhysicalDevice, m_Device);
        m_Allocator = VulkanAllocator::Get();
    }

    bool VulkanContext::CheckValidationLayerSupport()
    {
        uint32_t layerCount;
        vkEnumerateInstanceLayerProperties(&layerCount, nullptr);
        std::vector<VkLayerProperties> availableLayers(layerCount);
        vkEnumerateInstanceLayerProperties(&layerCount, availableLayers.data());

        for (const char* layerName : m_ValidationLayers) {
            bool layerFound = false;
            for (const auto& layerProperties : availableLayers) {
                if (strcmp(layerName, layerProperties.layerName) == 0) {
                    layerFound = true;
                    break;
                }
            }
            if (!layerFound) return false;
        }
        return true;
    }

    u32 VulkanContext::FindMemoryType(u32 typeFilter, VkMemoryPropertyFlags properties)
    {
        VkPhysicalDeviceMemoryProperties memProperties;
        vkGetPhysicalDeviceMemoryProperties(m_PhysicalDevice, &memProperties);

        for (uint32_t i = 0; i < memProperties.memoryTypeCount; i++) {
            if ((typeFilter & (1 << i)) && (memProperties.memoryTypes[i].propertyFlags & properties) == properties) {
                return i;
            }
        }
        LH_CORE_ASSERT(false, "Failed to find suitable memory type!");
        return 0;
    }

    void VulkanContext::ImmediateSubmit(std::function<void(VkCommandBuffer)>&& function)
    {
        LH_PROFILE_FUNCTION();

        VkCommandBufferAllocateInfo allocInfo{};
        allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        allocInfo.commandPool = m_CommandPool;
        allocInfo.commandBufferCount = 1;

        VkCommandBuffer commandBuffer;
        {
            std::lock_guard<std::mutex> lock(m_CommandPoolMutex);
            vkAllocateCommandBuffers(m_Device, &allocInfo, &commandBuffer);
        }

        VkCommandBufferBeginInfo beginInfo{};
        beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;

        vkBeginCommandBuffer(commandBuffer, &beginInfo);
        function(commandBuffer);
        vkEndCommandBuffer(commandBuffer);

        VkSubmitInfo submitInfo{};
        submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submitInfo.commandBufferCount = 1;
        submitInfo.pCommandBuffers = &commandBuffer;

        // Create a fence to wait for this specific submission
        VkFenceCreateInfo fenceInfo{};
        fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        VkFence fence;
        vkCreateFence(m_Device, &fenceInfo, nullptr, &fence);
        
        {
            std::lock_guard<std::mutex> lock(m_QueueMutex);
            vkQueueSubmit(m_GraphicsQueue, 1, &submitInfo, fence);
        }
        
        vkWaitForFences(m_Device, 1, &fence, VK_TRUE, UINT64_MAX);
        vkDestroyFence(m_Device, fence, nullptr);

        {
            std::lock_guard<std::mutex> lock(m_CommandPoolMutex);
            vkFreeCommandBuffers(m_Device, m_CommandPool, 1, &commandBuffer);
        }
    }

    bool VulkanContext::Submit(const VkSubmitInfo& submitInfo, VkFence fence)
    {
        std::lock_guard<std::mutex> lock(m_QueueMutex);
        if (vkQueueSubmit(m_GraphicsQueue, 1, &submitInfo, fence) != VK_SUCCESS)
        {
            LH_CORE_ERROR("VulkanContext: Queue Submit Failed!");
            return false;
        }
        return true;
    }

    VkResult VulkanContext::Present(const VkPresentInfoKHR& presentInfo)
    {
        std::lock_guard<std::mutex> lock(m_QueueMutex);
        return vkQueuePresentKHR(m_GraphicsQueue, &presentInfo);
    }

    void VulkanContext::PushDeletion(std::function<void()>&& function)
    {
        m_DeletionQueues[m_CurrentFrameIndex].deletors.push_back(function);
    }

    void VulkanContext::FlushDeletionQueue()
    {
        auto& queue = m_DeletionQueues[m_CurrentFrameIndex];
        for (auto& func : queue.deletors) {
            func();
        }
        queue.deletors.clear();
    }
}
