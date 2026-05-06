#include "luthpch.h"
#include "luth/renderer/backend/vulkan/VulkanContext.h"
#include "luth/renderer/backend/vulkan/VulkanAllocator.h"
#include "luth/renderer/backend/vulkan/UploadContext.h"
#include "luth/core/diagnostics/Log.h"

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

    // Shared by SetupDebugMessenger (persistent messenger) and CreateInstance (pNext-chained
    // temporary that captures vkCreateInstance / vkDestroyInstance failures — see VK_EXT_debug_utils).
    static void PopulateDebugMessengerCreateInfo(VkDebugUtilsMessengerCreateInfoEXT& ci)
    {
        ci = {};
        ci.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
        ci.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_VERBOSE_BIT_EXT |
                             VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
                             VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
        ci.messageType     = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
                             VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                             VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
        ci.pfnUserCallback = DebugCallback;
    }

    void VulkanContext::Init(void* windowHandle)
    {
        LH_CORE_ASSERT(!s_Instance, "VulkanContext already initialized!");
        s_Instance = LH_NEW(Memory::Category::Rendering, VulkanContext);
        s_Instance->m_WindowHandle = windowHandle;
        
        s_Instance->CreateInstance();
        s_Instance->SetupDebugMessenger();
        s_Instance->PickPhysicalDevice();
        s_Instance->CreateLogicalDevice();
        s_Instance->InitAllocator();
        s_Instance->m_BindlessSet.Init(s_Instance->m_Device);
        s_Instance->m_ResourceCache.Init();
        // Init last — needs the device, graphics queue, and VMA allocator. Consumed
        // immediately by VKVertexBuffer/VKIndexBuffer ctors during RenderingSystem startup.
        UploadContext::Init();
    }

    void VulkanContext::Shutdown()
    {
        if (!s_Instance) return;

        s_Instance->FlushAllDeletionQueues();

        // Shutdown order: UploadContext first — drains its timeline + frees the staging
        // VkBuffer while VMA + the device are still alive.
        UploadContext::Shutdown();
        s_Instance->m_ResourceCache.Shutdown();
        s_Instance->m_BindlessSet.Shutdown();
        VulkanAllocator::Shutdown();
        vkDestroyCommandPool(s_Instance->m_Device, s_Instance->m_CommandPool, nullptr);
        vkDestroyDevice(s_Instance->m_Device, nullptr);

        if (s_Instance->m_EnableValidationLayers) {
            auto func = (PFN_vkDestroyDebugUtilsMessengerEXT)vkGetInstanceProcAddr(s_Instance->m_Instance, "vkDestroyDebugUtilsMessengerEXT");
            if (func) func(s_Instance->m_Instance, s_Instance->m_DebugMessenger, nullptr);
        }

        vkDestroyInstance(s_Instance->m_Instance, nullptr);
        
        LH_DELETE(Memory::Category::Rendering, s_Instance);
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

        // Layers + pNext-chained debug messenger
        // The chained messenger is consumed by vkCreateInstance and is not retained — its lifetime
        // extends only until the call returns, which is exactly when we need it to catch instance
        // create/destroy failures (the persistent messenger from SetupDebugMessenger covers steady state).
        VkDebugUtilsMessengerCreateInfoEXT debugCI{};
        if (m_EnableValidationLayers) {
            createInfo.enabledLayerCount = static_cast<uint32_t>(m_ValidationLayers.size());
            createInfo.ppEnabledLayerNames = m_ValidationLayers.data();

            PopulateDebugMessengerCreateInfo(debugCI);
            createInfo.pNext = &debugCI;
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
        PopulateDebugMessengerCreateInfo(createInfo);

        auto func = (PFN_vkCreateDebugUtilsMessengerEXT)vkGetInstanceProcAddr(m_Instance, "vkCreateDebugUtilsMessengerEXT");
        if (func) {
            func(m_Instance, &createInfo, nullptr, &m_DebugMessenger);
        }
    }

    // Renderer baseline: VK_KHR_swapchain + a graphics queue family.
    static bool DeviceMeetsBaseline(VkPhysicalDevice device)
    {
        u32 extCount = 0;
        vkEnumerateDeviceExtensionProperties(device, nullptr, &extCount, nullptr);
        std::vector<VkExtensionProperties> extensions(extCount);
        vkEnumerateDeviceExtensionProperties(device, nullptr, &extCount, extensions.data());

        bool hasSwapchain = false;
        for (const auto& ext : extensions)
        {
            if (strcmp(ext.extensionName, VK_KHR_SWAPCHAIN_EXTENSION_NAME) == 0) { hasSwapchain = true; break; }
        }
        if (!hasSwapchain) return false;

        u32 famCount = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(device, &famCount, nullptr);
        std::vector<VkQueueFamilyProperties> families(famCount);
        vkGetPhysicalDeviceQueueFamilyProperties(device, &famCount, families.data());
        for (const auto& f : families)
            if (f.queueFlags & VK_QUEUE_GRAPHICS_BIT) return true;

        return false;
    }

    void VulkanContext::PickPhysicalDevice()
    {
        uint32_t deviceCount = 0;
        vkEnumeratePhysicalDevices(m_Instance, &deviceCount, nullptr);
        if (deviceCount == 0) LH_CORE_CRITICAL("Failed to find GPUs with Vulkan support!");

        std::vector<VkPhysicalDevice> devices(deviceCount);
        vkEnumeratePhysicalDevices(m_Instance, &deviceCount, devices.data());

        // Prefer discrete; fall back to first eligible. Surface-presentation support is checked in VulkanSwapchain.
        VkPhysicalDevice fallback = VK_NULL_HANDLE;
        for (const auto& device : devices)
        {
            if (!DeviceMeetsBaseline(device)) continue;

            VkPhysicalDeviceProperties props;
            vkGetPhysicalDeviceProperties(device, &props);
            if (props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU)
            {
                m_PhysicalDevice = device;
                m_PhysicalDeviceProperties = props;
                LH_CORE_INFO("Vulkan GPU: {0}", props.deviceName);
                return;
            }
            if (fallback == VK_NULL_HANDLE) fallback = device;
        }

        if (fallback != VK_NULL_HANDLE)
        {
            m_PhysicalDevice = fallback;
            VkPhysicalDeviceProperties props;
            vkGetPhysicalDeviceProperties(m_PhysicalDevice, &props);
            m_PhysicalDeviceProperties = props;
            LH_CORE_WARN("Vulkan GPU (non-discrete): {0}", props.deviceName);
            return;
        }

        LH_CORE_CRITICAL("No Vulkan device meets baseline (VK_KHR_swapchain + graphics queue)");
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

        // Verify required 1.1/1.2/1.3 features before enabling them in vkCreateDevice.
        VkPhysicalDeviceVulkan11Features avail11{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES };
        VkPhysicalDeviceVulkan12Features avail12{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES };
        VkPhysicalDeviceVulkan13Features avail13{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES };
        avail12.pNext = &avail11;
        avail13.pNext = &avail12;
        VkPhysicalDeviceFeatures2 avail2{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2 };
        avail2.pNext = &avail13;
        vkGetPhysicalDeviceFeatures2(m_PhysicalDevice, &avail2);

        const bool ok = avail11.shaderDrawParameters
                     && avail12.descriptorBindingPartiallyBound
                     && avail12.descriptorBindingSampledImageUpdateAfterBind
                     && avail12.descriptorBindingStorageBufferUpdateAfterBind
                     && avail12.descriptorBindingUniformBufferUpdateAfterBind
                     && avail12.runtimeDescriptorArray
                     && avail12.shaderSampledImageArrayNonUniformIndexing
                     && avail12.timelineSemaphore
                     && avail13.dynamicRendering
                     && avail13.synchronization2;
        if (!ok)
        {
            LH_CORE_CRITICAL("Required Vulkan 1.1/1.2/1.3 features missing on selected device — "
                "shaderDrawParameters={} descriptorBindingPartiallyBound={} "
                "descriptorBindingSampledImageUpdateAfterBind={} descriptorBindingStorageBufferUpdateAfterBind={} "
                "descriptorBindingUniformBufferUpdateAfterBind={} "
                "runtimeDescriptorArray={} shaderSampledImageArrayNonUniformIndexing={} timelineSemaphore={} "
                "dynamicRendering={} synchronization2={}",
                (bool)avail11.shaderDrawParameters,
                (bool)avail12.descriptorBindingPartiallyBound,
                (bool)avail12.descriptorBindingSampledImageUpdateAfterBind,
                (bool)avail12.descriptorBindingStorageBufferUpdateAfterBind,
                (bool)avail12.descriptorBindingUniformBufferUpdateAfterBind,
                (bool)avail12.runtimeDescriptorArray,
                (bool)avail12.shaderSampledImageArrayNonUniformIndexing,
                (bool)avail12.timelineSemaphore,
                (bool)avail13.dynamicRendering,
                (bool)avail13.synchronization2);
        }

        float queuePriority = 1.0f;
        VkDeviceQueueCreateInfo queueCreateInfo{};
        queueCreateInfo.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
        queueCreateInfo.queueFamilyIndex = m_GraphicsFamily;
        queueCreateInfo.queueCount = 1;
        queueCreateInfo.pQueuePriorities = &queuePriority;

        // Features
        VkPhysicalDeviceFeatures deviceFeatures{};
        deviceFeatures.samplerAnisotropy = VK_TRUE;
        deviceFeatures.fillModeNonSolid = VK_TRUE;
        deviceFeatures.independentBlend = VK_TRUE;

        // Vulkan 1.1 Features (Shader Draw Parameters for gl_BaseInstance)
        VkPhysicalDeviceVulkan11Features features11{};
        features11.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES;
        features11.shaderDrawParameters = VK_TRUE;

        // Vulkan 1.2 Features (Descriptor Indexing for Bindless)
        VkPhysicalDeviceVulkan12Features features12{};
        features12.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
        features12.pNext = &features11;
        features12.descriptorBindingPartiallyBound = VK_TRUE;
        features12.descriptorBindingSampledImageUpdateAfterBind = VK_TRUE;
        // Required for tagged-heap consumers whose descriptors are rewritten each frame
        // to point at fresh allocator regions: SSBOs (Set 2 Material / Set 4 Bones / Set 5 Object)
        // and UBOs (Set 0 Global+GTAO / Set 3 Light / PostProcess / Grid).
        features12.descriptorBindingStorageBufferUpdateAfterBind = VK_TRUE;
        features12.descriptorBindingUniformBufferUpdateAfterBind = VK_TRUE;
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
            VK_KHR_DYNAMIC_RENDERING_EXTENSION_NAME // Enabled for ImGui compatibility
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

    bool VulkanContext::Submit2(const VkSubmitInfo2& submitInfo, VkFence fence)
    {
        std::lock_guard<std::mutex> lock(m_QueueMutex);
        if (vkQueueSubmit2(m_GraphicsQueue, 1, &submitInfo, fence) != VK_SUCCESS)
        {
            LH_CORE_ERROR("VulkanContext: Queue Submit2 Failed!");
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
        SpinLockGuard lock(m_DeletionLock);
        m_DeletionQueues[m_CurrentFrameIndex].deletors.push_back(std::move(function));
    }

    void VulkanContext::FlushDeletionQueue()
    {
        // Drain under lock, run outside — a deletor may push (nested resource release).
        std::deque<std::function<void()>> drained;
        {
            SpinLockGuard lock(m_DeletionLock);
            drained.swap(m_DeletionQueues[m_CurrentFrameIndex].deletors);
        }
        for (auto& func : drained) func();
    }

    void VulkanContext::FlushAllDeletionQueues()
    {
        for (int i = 0; i < MAX_FRAMES_IN_FLIGHT; i++)
        {
            std::deque<std::function<void()>> drained;
            {
                SpinLockGuard lock(m_DeletionLock);
                drained.swap(m_DeletionQueues[i].deletors);
            }
            for (auto& func : drained) func();
        }
    }
}
