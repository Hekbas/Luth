#include "luthpch.h"
#include "luth/graphics/GfxContext.h"
#include "luth/core/Log.h"

#include <GLFW/glfw3.h>
#include <set>

namespace Luth::Gfx
{
    GfxContext* GfxContext::s_Instance = nullptr;

    static VKAPI_ATTR VkBool32 VKAPI_CALL DebugCallback(
        VkDebugUtilsMessageSeverityFlagBitsEXT messageSeverity,
        VkDebugUtilsMessageTypeFlagsEXT messageType,
        const VkDebugUtilsMessengerCallbackDataEXT* pCallbackData,
        void* pUserData)
    {
        if (messageSeverity >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT)
        {
            LH_CORE_ERROR("Vulkan: {0}", pCallbackData->pMessage);
        }
        return VK_FALSE;
    }

    void GfxContext::Init(void* windowHandle)
    {
        LH_CORE_ASSERT(!s_Instance, "GfxContext already initialized!");
        s_Instance = new GfxContext();
        
        s_Instance->CreateInstance();
        s_Instance->SetupDebugMessenger();
        s_Instance->CreateSurface(windowHandle);
        s_Instance->SelectPhysicalDevice();
        s_Instance->CreateLogicalDevice();
    }

    void GfxContext::Shutdown()
    {
        if (!s_Instance) return;

        vkDeviceWaitIdle(s_Instance->m_Device);

        vkDestroyDevice(s_Instance->m_Device, nullptr);
        vkDestroySurfaceKHR(s_Instance->m_Instance, s_Instance->m_Surface, nullptr);
        
        auto func = (PFN_vkDestroyDebugUtilsMessengerEXT)vkGetInstanceProcAddr(s_Instance->m_Instance, "vkDestroyDebugUtilsMessengerEXT");
        if (func) func(s_Instance->m_Instance, s_Instance->m_DebugMessenger, nullptr);

        vkDestroyInstance(s_Instance->m_Instance, nullptr);

        delete s_Instance;
        s_Instance = nullptr;
    }

    GfxContext& GfxContext::Get()
    {
        LH_CORE_ASSERT(s_Instance, "GfxContext not initialized!");
        return *s_Instance;
    }

    void GfxContext::CreateInstance()
    {
        VkApplicationInfo appInfo{};
        appInfo.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
        appInfo.pApplicationName = "Luth Engine";
        appInfo.applicationVersion = VK_MAKE_VERSION(1, 0, 0);
        appInfo.pEngineName = "Luth";
        appInfo.engineVersion = VK_MAKE_VERSION(1, 0, 0);
        appInfo.apiVersion = VK_API_VERSION_1_3;

        VkInstanceCreateInfo createInfo{};
        createInfo.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
        createInfo.pApplicationInfo = &appInfo;

        // Extensions
        uint32_t glfwExtensionCount = 0;
        const char** glfwExtensions = glfwGetRequiredInstanceExtensions(&glfwExtensionCount);
        std::vector<const char*> extensions(glfwExtensions, glfwExtensions + glfwExtensionCount);
        extensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);

        createInfo.enabledExtensionCount = (u32)extensions.size();
        createInfo.ppEnabledExtensionNames = extensions.data();

        // Layers
        const char* validationLayer = "VK_LAYER_KHRONOS_validation";
        createInfo.enabledLayerCount = 1;
        createInfo.ppEnabledLayerNames = &validationLayer;

        // Debug info for instance creation/destruction
        VkDebugUtilsMessengerCreateInfoEXT debugCreateInfo{};
        debugCreateInfo.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
        debugCreateInfo.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_VERBOSE_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
        debugCreateInfo.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
        debugCreateInfo.pfnUserCallback = DebugCallback;
        createInfo.pNext = &debugCreateInfo;

        if (vkCreateInstance(&createInfo, nullptr, &m_Instance) != VK_SUCCESS)
        {
            LH_CORE_CRITICAL("Failed to create Vulkan Instance!");
        }
    }

    void GfxContext::SetupDebugMessenger()
    {
        VkDebugUtilsMessengerCreateInfoEXT createInfo{};
        createInfo.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
        createInfo.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
        createInfo.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
        createInfo.pfnUserCallback = DebugCallback;

        auto func = (PFN_vkCreateDebugUtilsMessengerEXT)vkGetInstanceProcAddr(m_Instance, "vkCreateDebugUtilsMessengerEXT");
        if (func) func(m_Instance, &createInfo, nullptr, &m_DebugMessenger);
    }

    void GfxContext::CreateSurface(void* windowHandle)
    {
        if (glfwCreateWindowSurface(m_Instance, (GLFWwindow*)windowHandle, nullptr, &m_Surface) != VK_SUCCESS)
        {
            LH_CORE_CRITICAL("Failed to create Window Surface!");
        }
    }

    void GfxContext::SelectPhysicalDevice()
    {
        u32 deviceCount = 0;
        vkEnumeratePhysicalDevices(m_Instance, &deviceCount, nullptr);
        std::vector<VkPhysicalDevice> devices(deviceCount);
        vkEnumeratePhysicalDevices(m_Instance, &deviceCount, devices.data());

        for (const auto& device : devices)
        {
            VkPhysicalDeviceProperties props;
            vkGetPhysicalDeviceProperties(device, &props);

            if (props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU)
            {
                m_PhysicalDevice = device;
                LH_CORE_INFO("Selected GPU: {0}", props.deviceName);
                break;
            }
        }

        if (m_PhysicalDevice == VK_NULL_HANDLE && !devices.empty())
        {
            m_PhysicalDevice = devices[0];
            VkPhysicalDeviceProperties props;
            vkGetPhysicalDeviceProperties(m_PhysicalDevice, &props);
            LH_CORE_WARN("No Discrete GPU found, using: {0}", props.deviceName);
        }
    }

    void GfxContext::CreateLogicalDevice()
    {
        // Queue Families
        u32 queueFamilyCount = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(m_PhysicalDevice, &queueFamilyCount, nullptr);
        std::vector<VkQueueFamilyProperties> queueFamilies(queueFamilyCount);
        vkGetPhysicalDeviceQueueFamilyProperties(m_PhysicalDevice, &queueFamilyCount, queueFamilies.data());

        int graphicsFamily = -1;
        int presentFamily = -1;
        int computeFamily = -1;
        int transferFamily = -1;

        for (int i = 0; i < queueFamilies.size(); i++)
        {
            if (queueFamilies[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) graphicsFamily = i;
            if (queueFamilies[i].queueFlags & VK_QUEUE_COMPUTE_BIT) computeFamily = i;
            if (queueFamilies[i].queueFlags & VK_QUEUE_TRANSFER_BIT) transferFamily = i;
            
            VkBool32 presentSupport = false;
            vkGetPhysicalDeviceSurfaceSupportKHR(m_PhysicalDevice, i, m_Surface, &presentSupport);
            if (presentSupport) presentFamily = i;
        }

        std::set<int> uniqueQueueFamilies = { graphicsFamily, presentFamily };
        std::vector<VkDeviceQueueCreateInfo> queueCreateInfos;
        float queuePriority = 1.0f;

        for (int queueFamily : uniqueQueueFamilies)
        {
            VkDeviceQueueCreateInfo queueCreateInfo{};
            queueCreateInfo.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
            queueCreateInfo.queueFamilyIndex = queueFamily;
            queueCreateInfo.queueCount = 1;
            queueCreateInfo.pQueuePriorities = &queuePriority;
            queueCreateInfos.push_back(queueCreateInfo);
        }

        // Features
        VkPhysicalDeviceFeatures deviceFeatures{};
        deviceFeatures.samplerAnisotropy = VK_TRUE;
        deviceFeatures.fillModeNonSolid = VK_TRUE; // For wireframe
        deviceFeatures.wideLines = VK_TRUE;

        // Dynamic Rendering Features (Vulkan 1.3)
        VkPhysicalDeviceDynamicRenderingFeatures dynamicRendering{};
        dynamicRendering.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_FEATURES;
        dynamicRendering.dynamicRendering = VK_TRUE;

        // Synchronization 2 Features
        VkPhysicalDeviceSynchronization2Features sync2{};
        sync2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SYNCHRONIZATION_2_FEATURES;
        sync2.synchronization2 = VK_TRUE;
        sync2.pNext = &dynamicRendering;

        VkDeviceCreateInfo createInfo{};
        createInfo.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
        createInfo.pNext = &sync2; // Chain features
        createInfo.queueCreateInfoCount = (u32)queueCreateInfos.size();
        createInfo.pQueueCreateInfos = queueCreateInfos.data();
        createInfo.pEnabledFeatures = &deviceFeatures;

        const char* swapchainExt = VK_KHR_SWAPCHAIN_EXTENSION_NAME;
        createInfo.enabledExtensionCount = 1;
        createInfo.ppEnabledExtensionNames = &swapchainExt;

        if (vkCreateDevice(m_PhysicalDevice, &createInfo, nullptr, &m_Device) != VK_SUCCESS)
        {
            LH_CORE_CRITICAL("Failed to create Logical Device!");
        }

        // Get Queues
        vkGetDeviceQueue(m_Device, graphicsFamily, 0, &m_GraphicsQueue.handle);
        m_GraphicsQueue.familyIndex = graphicsFamily;

        vkGetDeviceQueue(m_Device, presentFamily, 0, &m_PresentQueue.handle);
        m_PresentQueue.familyIndex = presentFamily;
        
        // TODO: Get dedicated compute/transfer if available
        m_ComputeQueue = m_GraphicsQueue; 
        m_TransferQueue = m_GraphicsQueue;
    }

    u32 GfxContext::FindMemoryType(u32 typeFilter, VkMemoryPropertyFlags properties) const
    {
        VkPhysicalDeviceMemoryProperties memProperties;
        vkGetPhysicalDeviceMemoryProperties(m_PhysicalDevice, &memProperties);

        for (u32 i = 0; i < memProperties.memoryTypeCount; i++) {
            if ((typeFilter & (1 << i)) && (memProperties.memoryTypes[i].propertyFlags & properties) == properties) {
                return i;
            }
        }
        LH_CORE_ASSERT(false, "Failed to find suitable memory type!");
        return 0;
    }
}
