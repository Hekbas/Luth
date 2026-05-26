#include "luthpch.h"
#include "luth/renderer/backend/vulkan/VulkanContext.h"
#include "luth/renderer/backend/vulkan/VulkanAllocator.h"
#include "luth/renderer/backend/vulkan/UploadContext.h"
#include "luth/core/diagnostics/Log.h"

#include <GLFW/glfw3.h>

namespace Luth
{
    static VulkanContext* s_Instance = nullptr;

    void VulkanContext::SetDebugName(VkDescriptorSet set, const char* name)
    {
        if (!s_Instance || s_Instance->m_Device == VK_NULL_HANDLE || set == VK_NULL_HANDLE) return;
        static auto fn = (PFN_vkSetDebugUtilsObjectNameEXT)vkGetDeviceProcAddr(
            s_Instance->m_Device, "vkSetDebugUtilsObjectNameEXT");
        if (!fn) return;
        VkDebugUtilsObjectNameInfoEXT info{ VK_STRUCTURE_TYPE_DEBUG_UTILS_OBJECT_NAME_INFO_EXT };
        info.objectType   = VK_OBJECT_TYPE_DESCRIPTOR_SET;
        info.objectHandle = (u64)set;
        info.pObjectName  = name;
        fn(s_Instance->m_Device, &info);
    }

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

    // Renderer baseline: VK_KHR_swapchain + 4 RT extensions + a graphics queue family.
    // RT-mandatory (rt-renderer arc): a device missing any RT extension is ineligible,
    // not a fallback candidate — hard-fail at the picker rather than after vkCreateDevice.
    static bool DeviceMeetsBaseline(VkPhysicalDevice device)
    {
        u32 extCount = 0;
        vkEnumerateDeviceExtensionProperties(device, nullptr, &extCount, nullptr);
        std::vector<VkExtensionProperties> extensions(extCount);
        vkEnumerateDeviceExtensionProperties(device, nullptr, &extCount, extensions.data());

        const char* required[] = {
            VK_KHR_SWAPCHAIN_EXTENSION_NAME,
            VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME,
            VK_KHR_RAY_TRACING_PIPELINE_EXTENSION_NAME,
            VK_KHR_RAY_QUERY_EXTENSION_NAME,
            VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME,
        };
        for (const char* req : required)
        {
            bool found = false;
            for (const auto& ext : extensions)
                if (strcmp(ext.extensionName, req) == 0) { found = true; break; }
            if (!found) return false;
        }

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

        LH_CORE_CRITICAL("No Vulkan device meets baseline (VK_KHR_swapchain + RT extensions "
                         "[acceleration_structure, ray_tracing_pipeline, ray_query, deferred_host_operations] "
                         "+ graphics queue) — Luth is RT-mandatory per rt-renderer arc");
    }

    void VulkanContext::CreateLogicalDevice()
    {
        // Priority-order queue family discovery. Graphics is the baseline (asserted in PickPhysicalDevice).
        // Compute prefers a family with COMPUTE_BIT but no GRAPHICS_BIT (true async compute on discrete GPUs).
        // Transfer prefers a DMA-style family (TRANSFER_BIT, no GRAPHICS, no COMPUTE) so uploads run on a copy
        // engine in parallel with frame work. Fallbacks alias to graphics — single-family GPUs (Intel iGPU, etc.)
        // collapse to a single VkDeviceQueueCreateInfo and submit wrappers become no-cost dispatch.
        uint32_t queueFamilyCount = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(m_PhysicalDevice, &queueFamilyCount, nullptr);
        std::vector<VkQueueFamilyProperties> queueFamilies(queueFamilyCount);
        vkGetPhysicalDeviceQueueFamilyProperties(m_PhysicalDevice, &queueFamilyCount, queueFamilies.data());

        constexpr u32 kInvalid = (u32)-1;
        m_GraphicsFamily = kInvalid;
        m_ComputeFamily  = kInvalid;
        m_TransferFamily = kInvalid;

        for (u32 i = 0; i < queueFamilyCount; ++i)
        {
            const VkQueueFlags f = queueFamilies[i].queueFlags;
            if (m_GraphicsFamily == kInvalid && (f & VK_QUEUE_GRAPHICS_BIT))
                m_GraphicsFamily = i;
        }
        if (m_GraphicsFamily == kInvalid) LH_CORE_CRITICAL("Failed to find Graphics Queue Family!");

        // Async-compute pass: COMPUTE_BIT without GRAPHICS_BIT.
        for (u32 i = 0; i < queueFamilyCount; ++i)
        {
            const VkQueueFlags f = queueFamilies[i].queueFlags;
            if ((f & VK_QUEUE_COMPUTE_BIT) && !(f & VK_QUEUE_GRAPHICS_BIT))
            {
                m_ComputeFamily   = i;
                m_ComputeIsAsync  = true;
                break;
            }
        }
        if (m_ComputeFamily == kInvalid) m_ComputeFamily = m_GraphicsFamily;

        // DMA-style transfer: TRANSFER_BIT without GRAPHICS or COMPUTE. Then loosen to TRANSFER without GRAPHICS.
        for (u32 i = 0; i < queueFamilyCount; ++i)
        {
            const VkQueueFlags f = queueFamilies[i].queueFlags;
            if ((f & VK_QUEUE_TRANSFER_BIT) && !(f & VK_QUEUE_GRAPHICS_BIT) && !(f & VK_QUEUE_COMPUTE_BIT))
            {
                m_TransferFamily   = i;
                m_TransferIsAsync  = true;
                break;
            }
        }
        if (m_TransferFamily == kInvalid)
        {
            for (u32 i = 0; i < queueFamilyCount; ++i)
            {
                const VkQueueFlags f = queueFamilies[i].queueFlags;
                if ((f & VK_QUEUE_TRANSFER_BIT) && !(f & VK_QUEUE_GRAPHICS_BIT))
                {
                    m_TransferFamily   = i;
                    m_TransferIsAsync  = true;
                    break;
                }
            }
        }
        if (m_TransferFamily == kInvalid) m_TransferFamily = m_GraphicsFamily;

        // timestampValidBits compatibility: GPUTimerPool writes one shared query pool across all families using a
        // single device-level timestampPeriod. If valid bits diverge between families we use, conversion math is
        // wrong on the diverging queue. Rare on consumer hardware but the assertion catches it loudly.
        // See docs/development/arch/multi-queue.md (GPUTimerPool section) for the per-family-period future-polish path.
        const u32 graphicsBits = queueFamilies[m_GraphicsFamily].timestampValidBits;
        if (m_ComputeIsAsync)
        {
            const u32 b = queueFamilies[m_ComputeFamily].timestampValidBits;
            if (b != 0 && graphicsBits != 0 && b != graphicsBits)
                LH_CORE_CRITICAL("Compute family timestampValidBits ({}) differs from graphics ({}) — GPU timer "
                                 "math would corrupt on the compute stream; per-family period support not implemented.",
                                 b, graphicsBits);
        }
        if (m_TransferIsAsync)
        {
            const u32 b = queueFamilies[m_TransferFamily].timestampValidBits;
            if (b != 0 && graphicsBits != 0 && b != graphicsBits)
                LH_CORE_CRITICAL("Transfer family timestampValidBits ({}) differs from graphics ({}) — GPU timer "
                                 "math would corrupt on the transfer stream; per-family period support not implemented.",
                                 b, graphicsBits);
        }

        // Verify required 1.1/1.2/1.3 + RT features before enabling them in vkCreateDevice.
        // Chain shape: features2 -> 13 -> 12 -> 11 -> AS -> RT-pipeline -> ray-query.
        VkPhysicalDeviceVulkan11Features avail11{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES };
        VkPhysicalDeviceVulkan12Features avail12{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES };
        VkPhysicalDeviceVulkan13Features avail13{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES };
        VkPhysicalDeviceAccelerationStructureFeaturesKHR availAs{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR };
        VkPhysicalDeviceRayTracingPipelineFeaturesKHR    availRt{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_FEATURES_KHR };
        VkPhysicalDeviceRayQueryFeaturesKHR              availRq{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_QUERY_FEATURES_KHR };
        avail12.pNext = &avail11;
        avail13.pNext = &avail12;
        avail11.pNext = &availAs;
        availAs.pNext = &availRt;
        availRt.pNext = &availRq;
        VkPhysicalDeviceFeatures2 avail2{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2 };
        avail2.pNext = &avail13;
        vkGetPhysicalDeviceFeatures2(m_PhysicalDevice, &avail2);

        const bool ok = avail11.shaderDrawParameters
                     && avail12.descriptorBindingPartiallyBound
                     && avail12.descriptorBindingSampledImageUpdateAfterBind
                     && avail12.descriptorBindingStorageBufferUpdateAfterBind
                     && avail12.descriptorBindingStorageImageUpdateAfterBind
                     && avail12.descriptorBindingUniformBufferUpdateAfterBind
                     && avail12.runtimeDescriptorArray
                     && avail12.shaderSampledImageArrayNonUniformIndexing
                     && avail12.timelineSemaphore
                     && avail12.bufferDeviceAddress
                     && avail13.dynamicRendering
                     && avail13.synchronization2
                     && availAs.accelerationStructure
                     && availRt.rayTracingPipeline
                     && availRq.rayQuery;
        if (!ok)
        {
            LH_CORE_CRITICAL("Required Vulkan 1.1/1.2/1.3 + RT features missing on selected device — "
                "shaderDrawParameters={} descriptorBindingPartiallyBound={} "
                "descriptorBindingSampledImageUpdateAfterBind={} descriptorBindingStorageBufferUpdateAfterBind={} "
                "descriptorBindingStorageImageUpdateAfterBind={} descriptorBindingUniformBufferUpdateAfterBind={} "
                "runtimeDescriptorArray={} shaderSampledImageArrayNonUniformIndexing={} timelineSemaphore={} "
                "bufferDeviceAddress={} dynamicRendering={} synchronization2={} "
                "accelerationStructure={} rayTracingPipeline={} rayQuery={}",
                (bool)avail11.shaderDrawParameters,
                (bool)avail12.descriptorBindingPartiallyBound,
                (bool)avail12.descriptorBindingSampledImageUpdateAfterBind,
                (bool)avail12.descriptorBindingStorageBufferUpdateAfterBind,
                (bool)avail12.descriptorBindingStorageImageUpdateAfterBind,
                (bool)avail12.descriptorBindingUniformBufferUpdateAfterBind,
                (bool)avail12.runtimeDescriptorArray,
                (bool)avail12.shaderSampledImageArrayNonUniformIndexing,
                (bool)avail12.timelineSemaphore,
                (bool)avail12.bufferDeviceAddress,
                (bool)avail13.dynamicRendering,
                (bool)avail13.synchronization2,
                (bool)availAs.accelerationStructure,
                (bool)availRt.rayTracingPipeline,
                (bool)availRq.rayQuery);
        }

        // One VkDeviceQueueCreateInfo per distinct family. Up to 3 (graphics + async-compute + async-transfer);
        // collapses to 1 on single-family GPUs. Queue priorities are equal — Khronos sample-style priority inversion
        // is a tuning-pass future item (see arch/multi-queue.md).
        const float queuePriority = 1.0f;
        std::vector<u32> distinctFamilies;
        distinctFamilies.push_back(m_GraphicsFamily);
        if (m_ComputeIsAsync)  distinctFamilies.push_back(m_ComputeFamily);
        if (m_TransferIsAsync) distinctFamilies.push_back(m_TransferFamily);

        std::vector<VkDeviceQueueCreateInfo> queueCreateInfos;
        queueCreateInfos.reserve(distinctFamilies.size());
        for (u32 family : distinctFamilies)
        {
            VkDeviceQueueCreateInfo qci{};
            qci.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
            qci.queueFamilyIndex = family;
            qci.queueCount       = 1;
            qci.pQueuePriorities = &queuePriority;
            queueCreateInfos.push_back(qci);
        }

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
        // Required for tagged-heap consumers whose descriptors are rewritten each frame to point at fresh allocator
        // regions: SSBOs (Set 2 Material / Set 4 Bones / Set 5 Object) and UBOs (Set 0 Global+GTAO / Set 3 Light /
        // PostProcess / Grid). Storage-image variant required by GTAOMain layout (per-render-stage rewrites of the
        // output image binding under VUID 03047 race — see GTAOSubsystem.cpp's invariant comment).
        features12.descriptorBindingStorageBufferUpdateAfterBind = VK_TRUE;
        features12.descriptorBindingStorageImageUpdateAfterBind  = VK_TRUE;
        features12.descriptorBindingUniformBufferUpdateAfterBind = VK_TRUE;
        features12.runtimeDescriptorArray = VK_TRUE;
        features12.shaderSampledImageArrayNonUniformIndexing = VK_TRUE;

        // Enable Timeline Semaphores (Vulkan 1.2 feature)
        features12.timelineSemaphore = VK_TRUE;

        // BDA: vkGetBufferDeviceAddress + GLSL buffer_reference. VMA needs the matching
        // VMA_ALLOCATOR_CREATE_BUFFER_DEVICE_ADDRESS_BIT — see VulkanAllocator::Init.
        features12.bufferDeviceAddress = VK_TRUE;

        // Vulkan 1.3 Features (Dynamic Rendering, Synchronization2)
        VkPhysicalDeviceVulkan13Features features13{};
        features13.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
        features13.dynamicRendering = VK_TRUE;
        features13.synchronization2 = VK_TRUE;
        features13.pNext = &features12;

        // RT features (rt-renderer arc) — chain tail: 11 -> AS -> RT-pipeline -> ray-query.
        // accelerationStructure mandates bufferDeviceAddress (asserted enabled above).
        VkPhysicalDeviceAccelerationStructureFeaturesKHR asFeatures{};
        asFeatures.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR;
        asFeatures.accelerationStructure = VK_TRUE;

        VkPhysicalDeviceRayTracingPipelineFeaturesKHR rtPipelineFeatures{};
        rtPipelineFeatures.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_FEATURES_KHR;
        rtPipelineFeatures.rayTracingPipeline = VK_TRUE;

        VkPhysicalDeviceRayQueryFeaturesKHR rqFeatures{};
        rqFeatures.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_QUERY_FEATURES_KHR;
        rqFeatures.rayQuery = VK_TRUE;

        features11.pNext         = &asFeatures;
        asFeatures.pNext         = &rtPipelineFeatures;
        rtPipelineFeatures.pNext = &rqFeatures;

        VkDeviceCreateInfo createInfo{};
        createInfo.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
        createInfo.pNext = &features13; // Chain 1.3 features (full chain reaches RT structs)
        createInfo.pQueueCreateInfos    = queueCreateInfos.data();
        createInfo.queueCreateInfoCount = (u32)queueCreateInfos.size();
        createInfo.pEnabledFeatures = &deviceFeatures;

        std::vector<const char*> deviceExtensions = {
            VK_KHR_SWAPCHAIN_EXTENSION_NAME,
            VK_KHR_DYNAMIC_RENDERING_EXTENSION_NAME, // Enabled for ImGui compatibility
            VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME,
            VK_KHR_RAY_TRACING_PIPELINE_EXTENSION_NAME,
            VK_KHR_RAY_QUERY_EXTENSION_NAME,
            VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME,
        };

        createInfo.enabledExtensionCount = static_cast<uint32_t>(deviceExtensions.size());
        createInfo.ppEnabledExtensionNames = deviceExtensions.data();

        if (vkCreateDevice(m_PhysicalDevice, &createInfo, nullptr, &m_Device) != VK_SUCCESS) {
            LH_CORE_CRITICAL("Failed to create logical device!");
        }

        // KHR ray-tracing entry points are device-level — load right after vkCreateDevice,
        // before queue acquisition (loader doesn't depend on queues).
        LoadRayTracingFunctions();

        // Acquire queue handles. Distinct families each get their own queue; aliased families share the handle —
        // call sites use SubmitCompute2/SubmitTransfer2 either way, so the alias is invisible past this point.
        vkGetDeviceQueue(m_Device, m_GraphicsFamily, 0, &m_GraphicsQueue);
        m_ComputeQueue  = m_ComputeIsAsync  ? VK_NULL_HANDLE : m_GraphicsQueue;
        m_TransferQueue = m_TransferIsAsync ? VK_NULL_HANDLE : m_GraphicsQueue;
        if (m_ComputeIsAsync)  vkGetDeviceQueue(m_Device, m_ComputeFamily,  0, &m_ComputeQueue);
        if (m_TransferIsAsync) vkGetDeviceQueue(m_Device, m_TransferFamily, 0, &m_TransferQueue);

        // Deduped family list backs CONCURRENT-sharing resource creation. Already in canonical order: graphics first,
        // then async-compute (if distinct), then async-transfer (if distinct) — distinctFamilies was built that way.
        m_ConcurrentFamilyIndices = distinctFamilies;

        LH_CORE_INFO("Queue layout — graphics={}, compute={} ({}), transfer={} ({})",
            m_GraphicsFamily,
            m_ComputeFamily,  m_ComputeIsAsync  ? "async" : "aliased",
            m_TransferFamily, m_TransferIsAsync ? "async" : "aliased");

        // Command pool for ImmediateSubmit (graphics family — init-time IBL precompute, frame-debugger archive ops).
        VkCommandPoolCreateInfo poolInfo{};
        poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        poolInfo.queueFamilyIndex = m_GraphicsFamily;
        poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;

        vkCreateCommandPool(m_Device, &poolInfo, nullptr, &m_CommandPool);
    }

    void VulkanContext::LoadRayTracingFunctions()
    {
        // RT-mandatory: any missing fp is fatal. Validation-layer rules forbid silent fallback —
        // call sites would dispatch through null and trip the validation layer or crash at use site.
        #define LH_LOAD_RT_FN(field) \
            m_RtFn.field = (PFN_##field)vkGetDeviceProcAddr(m_Device, #field); \
            if (!m_RtFn.field) LH_CORE_CRITICAL("RT entry point missing: " #field)

        LH_LOAD_RT_FN(vkCreateAccelerationStructureKHR);
        LH_LOAD_RT_FN(vkDestroyAccelerationStructureKHR);
        LH_LOAD_RT_FN(vkGetAccelerationStructureBuildSizesKHR);
        LH_LOAD_RT_FN(vkCmdBuildAccelerationStructuresKHR);
        LH_LOAD_RT_FN(vkGetAccelerationStructureDeviceAddressKHR);
        LH_LOAD_RT_FN(vkCreateRayTracingPipelinesKHR);
        LH_LOAD_RT_FN(vkGetRayTracingShaderGroupHandlesKHR);
        LH_LOAD_RT_FN(vkCmdTraceRaysKHR);

        #undef LH_LOAD_RT_FN
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
        return SubmitGraphics2(submitInfo, fence);
    }

    bool VulkanContext::SubmitGraphics2(const VkSubmitInfo2& submitInfo, VkFence fence)
    {
        std::lock_guard<std::mutex> lock(m_QueueMutex);
        if (vkQueueSubmit2(m_GraphicsQueue, 1, &submitInfo, fence) != VK_SUCCESS)
        {
            LH_CORE_ERROR("VulkanContext: Graphics SubmitInfo2 Failed!");
            return false;
        }
        return true;
    }

    bool VulkanContext::SubmitCompute2(const VkSubmitInfo2& submitInfo, VkFence fence)
    {
        // Aliased compute queue still locks its own mutex — vkQueueSubmit2 is not re-entrant on the same VkQueue
        // even when handles match. Per-mutex prevents contention with concurrent graphics submits.
        std::lock_guard<std::mutex> lock(m_ComputeQueueMutex);
        if (vkQueueSubmit2(m_ComputeQueue, 1, &submitInfo, fence) != VK_SUCCESS)
        {
            LH_CORE_ERROR("VulkanContext: Compute SubmitInfo2 Failed!");
            return false;
        }
        return true;
    }

    bool VulkanContext::SubmitTransfer2(const VkSubmitInfo2& submitInfo, VkFence fence)
    {
        std::lock_guard<std::mutex> lock(m_TransferQueueMutex);
        if (vkQueueSubmit2(m_TransferQueue, 1, &submitInfo, fence) != VK_SUCCESS)
        {
            LH_CORE_ERROR("VulkanContext: Transfer SubmitInfo2 Failed!");
            return false;
        }
        return true;
    }

    VkResult VulkanContext::Present(const VkPresentInfoKHR& presentInfo)
    {
        std::lock_guard<std::mutex> lock(m_QueueMutex);
        return vkQueuePresentKHR(m_GraphicsQueue, &presentInfo);
    }

    void VulkanContext::ApplyConcurrentSharing(VkBufferCreateInfo& info) const
    {
        // Single-family layouts: leave EXCLUSIVE. CONCURRENT with one family index is implementation-defined
        // and validation-noisy; callers expect a graceful fallback on iGPU / single-queue hardware.
        if (m_ConcurrentFamilyIndices.size() <= 1) return;
        info.sharingMode           = VK_SHARING_MODE_CONCURRENT;
        info.queueFamilyIndexCount = (u32)m_ConcurrentFamilyIndices.size();
        info.pQueueFamilyIndices   = m_ConcurrentFamilyIndices.data();
    }

    void VulkanContext::ApplyConcurrentSharing(VkImageCreateInfo& info) const
    {
        if (m_ConcurrentFamilyIndices.size() <= 1) return;
        info.sharingMode           = VK_SHARING_MODE_CONCURRENT;
        info.queueFamilyIndexCount = (u32)m_ConcurrentFamilyIndices.size();
        info.pQueueFamilyIndices   = m_ConcurrentFamilyIndices.data();
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
