#include "Luthpch.h"
#include "luth/renderer/Renderer.h"
#include "luth/renderer/vulkan/VKRendererAPI.h"
#include "luth/renderer/vulkan/VKCommon.h"
#include "luth/renderer/vulkan/VKSwapchain.h"

#include <GLFW/glfw3.h>

namespace Luth
{
    // Debug callback integration with Luth logger
    static VKAPI_ATTR VkBool32 VKAPI_CALL DebugCallback(
        VkDebugUtilsMessageSeverityFlagBitsEXT messageSeverity,
        VkDebugUtilsMessageTypeFlagsEXT messageType,
        const VkDebugUtilsMessengerCallbackDataEXT* pCallbackData,
        void* pUserData)
    {
        switch (messageSeverity)
        {
            case VK_DEBUG_UTILS_MESSAGE_SEVERITY_VERBOSE_BIT_EXT:
                LH_CORE_TRACE("Vulkan Validation Layer: {0}", pCallbackData->pMessage);
                break;
            case VK_DEBUG_UTILS_MESSAGE_SEVERITY_INFO_BIT_EXT:
                LH_CORE_INFO("Vulkan Validation Layer: {0}", pCallbackData->pMessage);
                break;
            case VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT:
                LH_CORE_WARN("Vulkan Validation Layer: {0}", pCallbackData->pMessage);
                break;
            case VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT:
                LH_CORE_ERROR("Vulkan Validation Layer: {0}", pCallbackData->pMessage);
                break;
        }
        return VK_FALSE;
    }

    void VKRendererAPI::Init()
    {
        LH_CORE_INFO("Vulkan renderer init");

        CreateInstance();
        CreateSurface();

        if (m_EnableValidationLayers) {
            SetupDebugMessenger();
            PrintExtensions();
            PrintLayers();
        }

        CreateDevice();
        CreateSwapChain();
    }

    void VKRendererAPI::Shutdown()
    {
        if (m_Surface) {
            vkDestroySurfaceKHR(m_Instance, m_Surface, nullptr);
            LH_CORE_INFO("Vulkan surface destroyed");
        }
        if (m_Instance) {
            if (m_EnableValidationLayers) {
                DestroyDebugMessenger();
            }
            vkDestroyInstance(m_Instance, nullptr);
            m_Instance = VK_NULL_HANDLE;
            LH_CORE_INFO("Vulkan instance destroyed");
        }
    }

    void VKRendererAPI::SetViewport(u32 x, u32 y, u32 width, u32 height) {}

    void VKRendererAPI::SetClearColor(const glm::vec4& color) {}

    void VKRendererAPI::Clear() {}

    void VKRendererAPI::DrawIndexed(u32 count) {}

    void VKRendererAPI::CreateInstance()
    {
        if (m_EnableValidationLayers && !CheckValidationLayerSupport()) {
            LH_CORE_ASSERT(false, "Validation layers requested but not available!");
        }

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
        if (m_EnableValidationLayers) {
            extensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
        }

        createInfo.enabledExtensionCount = static_cast<uint32_t>(extensions.size());
        createInfo.ppEnabledExtensionNames = extensions.data();

        // Validation layers
        VkDebugUtilsMessengerCreateInfoEXT debugCreateInfo{};
        if (m_EnableValidationLayers) {
            createInfo.enabledLayerCount = static_cast<uint32_t>(m_ValidationLayers.size());
            createInfo.ppEnabledLayerNames = m_ValidationLayers.data();

            debugCreateInfo.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
            debugCreateInfo.messageSeverity =
                VK_DEBUG_UTILS_MESSAGE_SEVERITY_VERBOSE_BIT_EXT |
                VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
                VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
            debugCreateInfo.messageType =
                VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
                VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
            debugCreateInfo.pfnUserCallback = DebugCallback;
            createInfo.pNext = &debugCreateInfo;
        }
        else {
            createInfo.enabledLayerCount = 0;
            createInfo.pNext = nullptr;
        }

        VK_CHECK_RESULT(vkCreateInstance(&createInfo, nullptr, &m_Instance),
            "Failed to create Vulkan instance!");
    }

    void VKRendererAPI::CreateSurface()
    {
        GLFWwindow* glfwWindow = static_cast<GLFWwindow*>(s_Window);
        VK_CHECK_RESULT(glfwCreateWindowSurface(m_Instance, glfwWindow, nullptr, &m_Surface),
            "Failed to create window surface!");
    }

    void VKRendererAPI::CreateDevice()
    {
        m_PhysicalDevice = std::make_unique<VKPhysicalDevice>(m_Instance);

        if (!m_PhysicalDevice->IsSuitable()) {
            LH_CORE_CRITICAL("Physical device doesn't support required features!");
            return;
        }

        QueueFamilyIndices indices = m_PhysicalDevice->FindQueueFamilies();
        m_LogicalDevice = std::make_unique<VKLogicalDevice>(m_PhysicalDevice->GetHandle(), indices);
    }

    void VKRendererAPI::CreateSwapChain()
    {
        int width, height;
        glfwGetWindowSize(static_cast<GLFWwindow*>(s_Window), &width, &height);

        VKSwapchain::CreateInfo createInfo
        {
            .physicalDevice = m_PhysicalDevice->GetHandle(),
            .logicalDevice = m_LogicalDevice->GetHandle(),
            .surface = m_Surface,
            .width = (u32)width,
            .height = (u32)height
        };

        m_Swapchain = std::make_unique<VKSwapchain>(createInfo);
        
        LH_CORE_INFO("Swapchain created with {0} images",
            m_Swapchain->GetImageViews().size());
    }

    bool VKRendererAPI::CheckValidationLayerSupport() const
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

    void VKRendererAPI::PrintExtensions() const
    {
        uint32_t extensionCount = 0;
        vkEnumerateInstanceExtensionProperties(nullptr, &extensionCount, nullptr);
        std::vector<VkExtensionProperties> extensions(extensionCount);
        vkEnumerateInstanceExtensionProperties(nullptr, &extensionCount, extensions.data());

        LH_CORE_INFO("Available Vulkan Extensions:");
        for (const auto& extension : extensions) {
            LH_CORE_INFO("\t- {0}", extension.extensionName);
        }
    }

    void VKRendererAPI::PrintLayers() const
    {
        uint32_t layerCount = 0;
        vkEnumerateInstanceLayerProperties(&layerCount, nullptr);
        std::vector<VkLayerProperties> layers(layerCount);
        vkEnumerateInstanceLayerProperties(&layerCount, layers.data());

        LH_CORE_INFO("Available Vulkan Layers:");
        for (const auto& layer : layers) {
            LH_CORE_INFO("\t- {0} (v{1})",
                layer.layerName,
                VK_VERSION_MAJOR(layer.implementationVersion));
        }
    }

    void VKRendererAPI::SetupDebugMessenger()
    {
        auto func = (PFN_vkCreateDebugUtilsMessengerEXT)
            vkGetInstanceProcAddr(m_Instance, "vkCreateDebugUtilsMessengerEXT");
        if (!func) {
            LH_CORE_ASSERT(false, "Failed to load debug messenger extension!");
        }

        VkDebugUtilsMessengerCreateInfoEXT createInfo{};
        createInfo.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
        createInfo.messageSeverity =
            VK_DEBUG_UTILS_MESSAGE_SEVERITY_VERBOSE_BIT_EXT |
            VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
            VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
        createInfo.messageType =
            VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
            VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
            VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
        createInfo.pfnUserCallback = DebugCallback;

        VK_CHECK_RESULT(func(m_Instance, &createInfo, nullptr, &m_DebugMessenger),
            "Failed to set up debug messenger!");
    }

    void VKRendererAPI::DestroyDebugMessenger()
    {
        auto func = (PFN_vkDestroyDebugUtilsMessengerEXT)
            vkGetInstanceProcAddr(m_Instance, "vkDestroyDebugUtilsMessengerEXT");
        if (func) {
            func(m_Instance, m_DebugMessenger, nullptr);
        }
    }
}
