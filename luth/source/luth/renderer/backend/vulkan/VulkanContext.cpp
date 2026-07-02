#include "luthpch.h"
#include "luth/renderer/backend/vulkan/VulkanContext.h"
#include "luth/renderer/backend/vulkan/VulkanAllocator.h"
#include "luth/renderer/backend/vulkan/UploadContext.h"
#include "luth/renderer/backend/vulkan/GpuCheckpoint.h"
#include "luth/renderer/backend/vulkan/AftermathCrashTracker.h"
#include "luth/core/diagnostics/Log.h"

#include <GLFW/glfw3.h>

namespace Luth
{
    static VulkanContext* s_Instance = nullptr;

    void VulkanContext::SetDebugName(u64 objectHandle, VkObjectType type, const char* name)
    {
        if (!s_Instance || s_Instance->m_Device == VK_NULL_HANDLE || objectHandle == 0) return;
        auto fn = s_Instance->m_DebugUtilsFn.vkSetDebugUtilsObjectNameEXT;
        if (!fn) return;
        VkDebugUtilsObjectNameInfoEXT info{ VK_STRUCTURE_TYPE_DEBUG_UTILS_OBJECT_NAME_INFO_EXT };
        info.objectType   = type;
        info.objectHandle = objectHandle;
        info.pObjectName  = name;
        fn(s_Instance->m_Device, &info);
    }

    void VulkanContext::SetDebugName(VkImage h, const char* name)                    { SetDebugName((u64)h, VK_OBJECT_TYPE_IMAGE, name); }
    void VulkanContext::SetDebugName(VkImageView h, const char* name)                { SetDebugName((u64)h, VK_OBJECT_TYPE_IMAGE_VIEW, name); }
    void VulkanContext::SetDebugName(VkBuffer h, const char* name)                   { SetDebugName((u64)h, VK_OBJECT_TYPE_BUFFER, name); }
    void VulkanContext::SetDebugName(VkPipeline h, const char* name)                 { SetDebugName((u64)h, VK_OBJECT_TYPE_PIPELINE, name); }
    void VulkanContext::SetDebugName(VkDescriptorSet h, const char* name)            { SetDebugName((u64)h, VK_OBJECT_TYPE_DESCRIPTOR_SET, name); }
    void VulkanContext::SetDebugName(VkAccelerationStructureKHR h, const char* name) { SetDebugName((u64)h, VK_OBJECT_TYPE_ACCELERATION_STRUCTURE_KHR, name); }

    static VKAPI_ATTR VkBool32 VKAPI_CALL DebugCallback(
        VkDebugUtilsMessageSeverityFlagBitsEXT messageSeverity,
        VkDebugUtilsMessageTypeFlagsEXT messageType,
        const VkDebugUtilsMessengerCallbackDataEXT* pCallbackData,
        void* pUserData)
    {
        if (messageSeverity >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT)
            LH_LOG(Renderer, error, "Validation Layer: {0}", pCallbackData->pMessage);
        else if (messageSeverity >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT)
            LH_LOG(Renderer, warn, "Validation Layer: {0}", pCallbackData->pMessage);

        return VK_FALSE;
    }

    // Shared by SetupDebugMessenger (persistent messenger) and CreateInstance (pNext-chained
    // temporary that captures vkCreateInstance / vkDestroyInstance failures; see VK_EXT_debug_utils).
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
        
        // Enable Aftermath GPU crash dumps before any Vulkan object exists (no-op without the SDK).
        AftermathCrashTracker::Initialize();

        s_Instance->CreateInstance();
        s_Instance->SetupDebugMessenger();
        s_Instance->PickPhysicalDevice();
        s_Instance->CreateLogicalDevice();
        s_Instance->InitAllocator();
        s_Instance->m_BindlessSet.Init(s_Instance->m_Device);
        s_Instance->m_ResourceCache.Init();
        s_Instance->InitGpuProfilerContexts();  // Tracy GPU contexts; needs the device + queues
        // Init last; needs the device, graphics queue, and VMA allocator. Consumed
        // immediately by VKVertexBuffer/VKIndexBuffer ctors during RenderingSystem startup.
        UploadContext::Init();
    }

    void VulkanContext::Shutdown()
    {
        if (!s_Instance) return;

        s_Instance->FlushAllDeletionQueues();

        // Shutdown order: UploadContext first; drains its timeline + frees the staging VkBuffer while VMA + the
        // device are still alive.
        UploadContext::Shutdown();
        s_Instance->m_ResourceCache.Shutdown();
        s_Instance->m_BindlessSet.Shutdown();
        VulkanAllocator::Shutdown();
        s_Instance->ShutdownGpuProfilerContexts();  // destroys the Tracy query pools while the device is alive
        vkDestroyCommandPool(s_Instance->m_Device, s_Instance->m_CommandPool, nullptr);
        vkDestroyDevice(s_Instance->m_Device, nullptr);

        AftermathCrashTracker::Shutdown();

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

#if defined(TRACY_ENABLE)
    // The transient pool + buffer exist only so TracyVkContext can submit+wait a calibration timestamp;
    // both are freed immediately after. Init is single-threaded, so the raw queue use needs no mutex.
    static GpuTracyCtx CreateTracyCtxForQueue(VkDevice device, VkPhysicalDevice physDev, VkQueue queue,
                                              u32 family, const char* name)
    {
        VkCommandPoolCreateInfo poolCI{ VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
        poolCI.queueFamilyIndex = family;
        poolCI.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT | VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        VkCommandPool pool = VK_NULL_HANDLE;
        vkCreateCommandPool(device, &poolCI, nullptr, &pool);

        VkCommandBufferAllocateInfo cbAI{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
        cbAI.commandPool        = pool;
        cbAI.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        cbAI.commandBufferCount = 1;
        VkCommandBuffer cmd = VK_NULL_HANDLE;
        vkAllocateCommandBuffers(device, &cbAI, &cmd);

        GpuTracyCtx ctx = LH_PROFILE_GPU_CONTEXT(physDev, device, queue, cmd);
        LH_PROFILE_GPU_CONTEXT_NAME(ctx, name, (u16)strlen(name));

        vkDestroyCommandPool(device, pool, nullptr);
        return ctx;
    }
#endif

    void VulkanContext::InitGpuProfilerContexts()
    {
    #if defined(TRACY_ENABLE)
        m_GraphicsTracyCtx = CreateTracyCtxForQueue(m_Device, m_PhysicalDevice, m_GraphicsQueue,
                                                    m_GraphicsFamily, "GPU Graphics");
        // Single-family GPUs share one queue: route compute-pass zones to the graphics context.
        m_ComputeTracyCtx = m_ComputeIsAsync
            ? CreateTracyCtxForQueue(m_Device, m_PhysicalDevice, m_ComputeQueue, m_ComputeFamily, "GPU Compute")
            : m_GraphicsTracyCtx;
    #endif
    }

    void VulkanContext::ShutdownGpuProfilerContexts()
    {
    #if defined(TRACY_ENABLE)
        if (m_ComputeTracyCtx && m_ComputeTracyCtx != m_GraphicsTracyCtx)
            LH_PROFILE_GPU_DESTROY(m_ComputeTracyCtx);
        if (m_GraphicsTracyCtx)
            LH_PROFILE_GPU_DESTROY(m_GraphicsTracyCtx);
        m_ComputeTracyCtx  = nullptr;
        m_GraphicsTracyCtx = nullptr;
    #endif
    }

    void VulkanContext::ResolveValidationConfig()
    {
        // Default tier (also empty / "1" / "on" / "default"): core + sync-val + best-practices. GPU-AV
        // is excluded by default: its instrumentation perturbs submit timing and can mask races.
        // see arch/gpu-crash-debugging.md
        auto applyDefault = [this]{ m_ValTiers = {}; m_ValTiers.sync = true; m_ValTiers.bestPractices = true; };

        const char* env = std::getenv("LUTH_VALIDATION");
        if (!env) {                                  // no override: honor the BuildConfig decision
            if (m_EnableValidationLayers) applyDefault();
            return;
        }

        std::string s = env;
        for (char& c : s) if (c >= 'A' && c <= 'Z') c = char(c + 32);

        if (s == "off" || s == "0" || s == "none") { m_EnableValidationLayers = false; m_ValTiers = {}; return; }

        m_EnableValidationLayers = true;             // any non-off value forces validation on, any build
        m_ValTiers = {};
        if (s.empty() || s == "1" || s == "on" || s == "default") { applyDefault(); return; }
        if (s == "all") { m_ValTiers = { true, true, true, true, true }; return; }

        for (size_t i = 0; i < s.size(); ) {         // token list split on , ; or space
            size_t j = s.find_first_of(",; ", i);
            if (j == std::string::npos) j = s.size();
            std::string tok = s.substr(i, j - i);
            i = j + 1;
            if      (tok.empty() || tok == "core")          {}                              // core is implied
            else if (tok == "sync")                         m_ValTiers.sync = true;
            else if (tok == "bp" || tok == "bestpractices") m_ValTiers.bestPractices = true;
            else if (tok == "gpuav" || tok == "gpu")        m_ValTiers.gpuav = true;
            else if (tok == "rt")                           m_ValTiers.rtValidation = true;
            else if (tok == "uncapped" || tok == "verbose") m_ValTiers.uncapped = true;
            else LH_LOG(Renderer, warn, "LUTH_VALIDATION: unknown tier '{}' (sync|bp|gpuav|rt|uncapped|all|off)", tok);
        }
    }

    void VulkanContext::CreateInstance()
    {
        // LUTH_VALIDATION (any build) overrides the BuildConfig default + selects feature tiers, so a
        // Release binary can enable the validation stack without a rebuild to diagnose Release-only GPU
        // faults. Needs the Vulkan SDK layers present (soft-fail below).
        ResolveValidationConfig();

        if (m_EnableValidationLayers && !CheckValidationLayerSupport()) {
            LH_LOG(Renderer, error, "Validation layers requested, but not available!");
            m_EnableValidationLayers = false;
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

        bool haveLayerSettings = false;
        if (m_EnableValidationLayers) {
            extensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);

            // VK_EXT_layer_settings (from the validation layer) lifts the duplicate-message cap to
            // unlimited so a per-frame hazard stays visible past its 10th hit. Only when 'uncapped' set.
            if (m_ValTiers.uncapped) {
                u32 lsCount = 0;
                vkEnumerateInstanceExtensionProperties("VK_LAYER_KHRONOS_validation", &lsCount, nullptr);
                std::vector<VkExtensionProperties> lsExts(lsCount);
                vkEnumerateInstanceExtensionProperties("VK_LAYER_KHRONOS_validation", &lsCount, lsExts.data());
                for (const auto& e : lsExts)
                    if (strcmp(e.extensionName, VK_EXT_LAYER_SETTINGS_EXTENSION_NAME) == 0) { haveLayerSettings = true; break; }
                if (haveLayerSettings)
                    extensions.push_back(VK_EXT_LAYER_SETTINGS_EXTENSION_NAME);
            }
        }

        createInfo.enabledExtensionCount = static_cast<uint32_t>(extensions.size());
        createInfo.ppEnabledExtensionNames = extensions.data();

        // Layers + pNext-chained debug messenger
        // The chained messenger is consumed by vkCreateInstance and is not retained; its lifetime
        // extends only until the call returns, exactly the window that catches instance
        // create/destroy failures (the persistent messenger from SetupDebugMessenger covers steady state).
        VkDebugUtilsMessengerCreateInfoEXT debugCI{};
        // Validation features selected per tier. Legacy VkValidationFeaturesEXT route, still functional;
        // the modern path is VK_EXT_layer_settings (validate_sync / gpuav_enable), see
        // arch/gpu-crash-debugging.md. GPU-AV must list GPU_ASSISTED before RESERVE_BINDING_SLOT (its VU).
        std::vector<VkValidationFeatureEnableEXT> valFeatures;
        if (m_ValTiers.sync)  valFeatures.push_back(VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT);
        if (m_ValTiers.gpuav) {
            valFeatures.push_back(VK_VALIDATION_FEATURE_ENABLE_GPU_ASSISTED_EXT);
            valFeatures.push_back(VK_VALIDATION_FEATURE_ENABLE_GPU_ASSISTED_RESERVE_BINDING_SLOT_EXT);
        }
        if (m_ValTiers.bestPractices) valFeatures.push_back(VK_VALIDATION_FEATURE_ENABLE_BEST_PRACTICES_EXT);
        if (m_ValTiers.gpuav)
            LH_LOG(Renderer, warn, "LUTH_VALIDATION: GPU-AV on - perturbs submit timing (can mask races) and "
                         "consumes a descriptor set (maxBoundDescriptorSets-1)");

        VkValidationFeaturesEXT validationFeatures{ VK_STRUCTURE_TYPE_VALIDATION_FEATURES_EXT };
        validationFeatures.enabledValidationFeatureCount = static_cast<uint32_t>(valFeatures.size());
        validationFeatures.pEnabledValidationFeatures    = valFeatures.data();

        // duplicate_message_limit = 0 (unlimited) so capped per-frame hazards stay visible (see probe above).
        const uint32_t kDupLimit = 0;
        const VkLayerSettingEXT layerSettingArr[] = {
            { "VK_LAYER_KHRONOS_validation", "duplicate_message_limit",
              VK_LAYER_SETTING_TYPE_UINT32_EXT, 1, &kDupLimit },
        };
        VkLayerSettingsCreateInfoEXT layerSettingsCI{ VK_STRUCTURE_TYPE_LAYER_SETTINGS_CREATE_INFO_EXT };
        layerSettingsCI.settingCount = static_cast<uint32_t>(std::size(layerSettingArr));
        layerSettingsCI.pSettings    = layerSettingArr;

        if (m_EnableValidationLayers) {
            createInfo.enabledLayerCount = static_cast<uint32_t>(m_ValidationLayers.size());
            createInfo.ppEnabledLayerNames = m_ValidationLayers.data();

            // pNext chain (tail -> head): debugCI -> [validationFeatures] -> [layerSettings].
            PopulateDebugMessengerCreateInfo(debugCI);
            const void* chain = &debugCI;
            if (!valFeatures.empty()) { validationFeatures.pNext = chain; chain = &validationFeatures; }
            if (haveLayerSettings)    { layerSettingsCI.pNext   = chain; chain = &layerSettingsCI; }
            createInfo.pNext = chain;
        } else {
            createInfo.enabledLayerCount = 0;
        }

        if (vkCreateInstance(&createInfo, nullptr, &m_Instance) != VK_SUCCESS) {
            LH_LOG(Renderer, critical, "Failed to create Vulkan instance!");
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
    // RT-mandatory: a device missing any RT extension is ineligible, not a fallback
    // candidate; hard-fail at the picker rather than after vkCreateDevice.
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
        if (deviceCount == 0) LH_LOG(Renderer, critical, "Failed to find GPUs with Vulkan support!");

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
                LH_LOG(Renderer, info, "Vulkan GPU: {0}", props.deviceName);
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
            LH_LOG(Renderer, warn, "Vulkan GPU (non-discrete): {0}", props.deviceName);
            return;
        }

        LH_LOG(Renderer, critical, "No Vulkan device meets baseline (VK_KHR_swapchain + RT extensions "
                         "[acceleration_structure, ray_tracing_pipeline, ray_query, deferred_host_operations] "
                         "+ graphics queue) — Luth is RT-mandatory per rt-renderer arc");
    }

    void VulkanContext::CreateLogicalDevice()
    {
        // RT properties (shaderGroup sizes + alignments + recursion depth) are vendor-dependent;
        // queried once here so every RT consumer (SBT builder, BLAS sizing, RT pipeline) reads from
        // one cached struct on VulkanContext. PickPhysicalDevice has two return paths and would
        // duplicate the call; CreateLogicalDevice runs once after the picker settles.
        m_RtPipelineProperties.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_PROPERTIES_KHR;
        m_AsProperties.sType         = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_PROPERTIES_KHR;
        m_RtPipelineProperties.pNext = &m_AsProperties;
        VkPhysicalDeviceProperties2 props2{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2 };
        props2.pNext = &m_RtPipelineProperties;
        vkGetPhysicalDeviceProperties2(m_PhysicalDevice, &props2);

        LH_LOG(Renderer, info, "RT: shaderGroupHandleSize={} baseAlignment={} handleAlignment={} maxRecursionDepth={} maxGeometryCount={}",
            m_RtPipelineProperties.shaderGroupHandleSize,
            m_RtPipelineProperties.shaderGroupBaseAlignment,
            m_RtPipelineProperties.shaderGroupHandleAlignment,
            m_RtPipelineProperties.maxRayRecursionDepth,
            m_AsProperties.maxGeometryCount);

        // Priority-order queue family discovery. Graphics is the baseline (asserted in PickPhysicalDevice).
        // Compute prefers a family with COMPUTE_BIT but no GRAPHICS_BIT (true async compute on discrete GPUs).
        // Transfer prefers a DMA-style family (TRANSFER_BIT, no GRAPHICS, no COMPUTE) so uploads run on a copy
        // engine in parallel with frame work. Fallbacks alias to graphics; single-family GPUs (Intel iGPU, etc.)
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
        if (m_GraphicsFamily == kInvalid) LH_LOG(Renderer, critical, "Failed to find Graphics Queue Family!");

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
        // single device-level timestampPeriod. If valid bits diverge between the families in use, conversion math is
        // wrong on the diverging queue. Rare on consumer hardware but the assertion catches it loudly.
        // See docs/development/arch/multi-queue.md (GPUTimerPool section) for the per-family-period future-polish path.
        const u32 graphicsBits = queueFamilies[m_GraphicsFamily].timestampValidBits;
        if (m_ComputeIsAsync)
        {
            const u32 b = queueFamilies[m_ComputeFamily].timestampValidBits;
            if (b != 0 && graphicsBits != 0 && b != graphicsBits)
                LH_LOG(Renderer, critical, "Compute family timestampValidBits ({}) differs from graphics ({}) — GPU timer "
                                 "math would corrupt on the compute stream; per-family period support not implemented.",
                                 b, graphicsBits);
        }
        if (m_TransferIsAsync)
        {
            const u32 b = queueFamilies[m_TransferFamily].timestampValidBits;
            if (b != 0 && graphicsBits != 0 && b != graphicsBits)
                LH_LOG(Renderer, critical, "Transfer family timestampValidBits ({}) differs from graphics ({}) — GPU timer "
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
                     && avail12.scalarBlockLayout
                     && avail13.dynamicRendering
                     && avail13.synchronization2
                     && availAs.accelerationStructure
                     && availAs.descriptorBindingAccelerationStructureUpdateAfterBind
                     && availRt.rayTracingPipeline
                     && availRq.rayQuery;
        if (!ok)
        {
            LH_LOG(Renderer, critical, "Required Vulkan 1.1/1.2/1.3 + RT features missing on selected device — "
                "shaderDrawParameters={} descriptorBindingPartiallyBound={} "
                "descriptorBindingSampledImageUpdateAfterBind={} descriptorBindingStorageBufferUpdateAfterBind={} "
                "descriptorBindingStorageImageUpdateAfterBind={} descriptorBindingUniformBufferUpdateAfterBind={} "
                "runtimeDescriptorArray={} shaderSampledImageArrayNonUniformIndexing={} timelineSemaphore={} "
                "bufferDeviceAddress={} scalarBlockLayout={} dynamicRendering={} synchronization2={} "
                "accelerationStructure={} asUpdateAfterBind={} rayTracingPipeline={} rayQuery={}",
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
                (bool)avail12.scalarBlockLayout,
                (bool)avail13.dynamicRendering,
                (bool)avail13.synchronization2,
                (bool)availAs.accelerationStructure,
                (bool)availAs.descriptorBindingAccelerationStructureUpdateAfterBind,
                (bool)availRt.rayTracingPipeline,
                (bool)availRq.rayQuery);
        }

        // One VkDeviceQueueCreateInfo per distinct family. Up to 3 (graphics + async-compute + async-transfer);
        // collapses to 1 on single-family GPUs. Queue priorities are equal; Khronos sample-style priority inversion
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
        // Per-pass GPU pipeline statistics (overdraw / geometry counts) for the editor profiler, enabled
        // only when supported; spanning secondary cmd buffers also needs inheritedQueries. GPUTimerPool gates
        // collection on SupportsPipelineStats(), so an unsupported GPU degrades cleanly to timing-only.
        deviceFeatures.pipelineStatisticsQuery = avail2.features.pipelineStatisticsQuery;
        deviceFeatures.inheritedQueries        = avail2.features.inheritedQueries;
        m_PipelineStatsSupported = avail2.features.pipelineStatisticsQuery && avail2.features.inheritedQueries;

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
        // output image binding under VUID 03047 race; see GTAOSubsystem.cpp's invariant comment).
        features12.descriptorBindingStorageBufferUpdateAfterBind = VK_TRUE;
        features12.descriptorBindingStorageImageUpdateAfterBind  = VK_TRUE;
        features12.descriptorBindingUniformBufferUpdateAfterBind = VK_TRUE;
        features12.runtimeDescriptorArray = VK_TRUE;
        features12.shaderSampledImageArrayNonUniformIndexing = VK_TRUE;

        // Enable Timeline Semaphores (Vulkan 1.2 feature)
        features12.timelineSemaphore = VK_TRUE;

        // BDA: vkGetBufferDeviceAddress + GLSL buffer_reference. VMA needs the matching
        // VMA_ALLOCATOR_CREATE_BUFFER_DEVICE_ADDRESS_BIT; see VulkanAllocator::Init.
        features12.bufferDeviceAddress = VK_TRUE;

        // scalarBlockLayout: skinning.slang reads the tight 84 B SkinnedVertex VB directly via a scalar
        // buffer_reference (no padded skin-input copy).
        features12.scalarBlockLayout = VK_TRUE;

        // Vulkan 1.3 Features (Dynamic Rendering, Synchronization2)
        VkPhysicalDeviceVulkan13Features features13{};
        features13.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
        features13.dynamicRendering = VK_TRUE;
        features13.synchronization2 = VK_TRUE;
        features13.pNext = &features12;

        // RT features. Chain tail: 11 -> AS -> RT-pipeline -> ray-query.
        // accelerationStructure mandates bufferDeviceAddress (asserted enabled above).
        VkPhysicalDeviceAccelerationStructureFeaturesKHR asFeatures{};
        asFeatures.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR;
        asFeatures.accelerationStructure = VK_TRUE;
        // Required so Set 0 binding 6 (TLAS) can use UPDATE_AFTER_BIND; VUID-03570 fires on layout
        // create without it. Asserted in the baseline check above so RT-mandatory devices guarantee it.
        asFeatures.descriptorBindingAccelerationStructureUpdateAfterBind = VK_TRUE;

        VkPhysicalDeviceRayTracingPipelineFeaturesKHR rtPipelineFeatures{};
        rtPipelineFeatures.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_FEATURES_KHR;
        rtPipelineFeatures.rayTracingPipeline = VK_TRUE;

        VkPhysicalDeviceRayQueryFeaturesKHR rqFeatures{};
        rqFeatures.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_QUERY_FEATURES_KHR;
        rqFeatures.rayQuery = VK_TRUE;

        // NV ray-tracing validation (driver-level AS-build / SBT / shader-type checks). Chained into
        // rqFeatures.pNext below iff the extension is present + validation is on; rayTracingValidation
        // stays VK_FALSE otherwise. Declared here so it outlives vkCreateDevice.
        VkPhysicalDeviceRayTracingValidationFeaturesNV rtValidationFeatures{
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_VALIDATION_FEATURES_NV };

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

        // Enumerate device extensions once for the optional-diagnostic probes below.
        u32 availCount = 0;
        vkEnumerateDeviceExtensionProperties(m_PhysicalDevice, nullptr, &availCount, nullptr);
        std::vector<VkExtensionProperties> available(availCount);
        vkEnumerateDeviceExtensionProperties(m_PhysicalDevice, nullptr, &availCount, available.data());
        auto hasDeviceExt = [&available](const char* name) {
            for (const auto& e : available)
                if (strcmp(e.extensionName, name) == 0) return true;
            return false;
        };

        // Optional diagnostic (NV-only): localizes the failing GPU command after TDR.
        // Absence is a soft-fail (the dump path checks HasCheckpoints() before invoking).
        if (hasDeviceExt(VK_NV_DEVICE_DIAGNOSTIC_CHECKPOINTS_EXTENSION_NAME))
        {
            deviceExtensions.push_back(VK_NV_DEVICE_DIAGNOSTIC_CHECKPOINTS_EXTENSION_NAME);
            m_CheckpointsAvailable = true;
            LH_LOG(Renderer, info, "VK_NV_device_diagnostic_checkpoints enabled - TDR localization active");
        }
        else
        {
            LH_LOG(Renderer, info, "VK_NV_device_diagnostic_checkpoints unavailable on this device");
        }

        // NVIDIA driver-level ray-tracing validation: catches malformed AS builds (degenerate / OOB
        // geometry), bad SBT, unexpected shader types. Opt in via LUTH_VALIDATION=rt (which forces the
        // validation layer on, since it reports through the VK_EXT_debug_utils messenger). The driver
        // only reports the extension when the NV_ALLOW_RAYTRACING_VALIDATION=1 environment var is also set.
        if (m_ValTiers.rtValidation && hasDeviceExt(VK_NV_RAY_TRACING_VALIDATION_EXTENSION_NAME))
        {
            deviceExtensions.push_back(VK_NV_RAY_TRACING_VALIDATION_EXTENSION_NAME);
            rtValidationFeatures.rayTracingValidation = VK_TRUE;
            rqFeatures.pNext = &rtValidationFeatures;  // extend the feature chain tail
            LH_LOG(Renderer, info, "VK_NV_ray_tracing_validation enabled - RT AS/SBT validation active");
        }
        else if (m_ValTiers.rtValidation)
        {
            LH_LOG(Renderer, info, "VK_NV_ray_tracing_validation unavailable (set NV_ALLOW_RAYTRACING_VALIDATION=1)");
        }

#if defined(LUTH_ENABLE_AFTERMATH)
        // Aftermath: resource tracking + automatic checkpoints + shader debug info, so the GPU crash
        // dump can name the faulting resource/shader. Prepended to the device pNext chain; structs are
        // function-scoped so they outlive vkCreateDevice below.
        VkPhysicalDeviceDiagnosticsConfigFeaturesNV diagFeatures{
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DIAGNOSTICS_CONFIG_FEATURES_NV };
        VkDeviceDiagnosticsConfigCreateInfoNV diagConfig{
            VK_STRUCTURE_TYPE_DEVICE_DIAGNOSTICS_CONFIG_CREATE_INFO_NV };
        // Only enable driver-side tracking when Aftermath actually loaded; no wasted overhead on a
        // soft-fail (missing DLL). AUTOMATIC_CHECKPOINTS is intentionally omitted: NVIDIA rates it very
        // high CPU (per-command call-stack walk) and the per-pass vkCmdSetCheckpointNV markers in
        // RenderGraph already localize the failing pass far cheaper. see arch/gpu-crash-debugging.md
        if (AftermathCrashTracker::Enabled() && hasDeviceExt(VK_NV_DEVICE_DIAGNOSTICS_CONFIG_EXTENSION_NAME))
        {
            deviceExtensions.push_back(VK_NV_DEVICE_DIAGNOSTICS_CONFIG_EXTENSION_NAME);
            diagFeatures.diagnosticsConfig = VK_TRUE;
            diagConfig.flags = VK_DEVICE_DIAGNOSTICS_CONFIG_ENABLE_RESOURCE_TRACKING_BIT_NV
                             | VK_DEVICE_DIAGNOSTICS_CONFIG_ENABLE_SHADER_DEBUG_INFO_BIT_NV
                             | VK_DEVICE_DIAGNOSTICS_CONFIG_ENABLE_SHADER_ERROR_REPORTING_BIT_NV;
            diagFeatures.pNext = const_cast<void*>(createInfo.pNext);
            diagConfig.pNext   = &diagFeatures;
            createInfo.pNext   = &diagConfig;
            LH_LOG(Renderer, info, "VK_NV_device_diagnostics_config enabled - Aftermath resource tracking active");
        }
#endif

        createInfo.enabledExtensionCount = static_cast<uint32_t>(deviceExtensions.size());
        createInfo.ppEnabledExtensionNames = deviceExtensions.data();

        if (vkCreateDevice(m_PhysicalDevice, &createInfo, nullptr, &m_Device) != VK_SUCCESS) {
            LH_LOG(Renderer, critical, "Failed to create logical device!");
        }

        // KHR ray-tracing entry points are device-level; load right after vkCreateDevice, before queue
        // acquisition (loader doesn't depend on queues).
        LoadRayTracingFunctions();
        if (m_CheckpointsAvailable) LoadCheckpointFunctions();
        LoadDebugUtilsFunctions();

        // Acquire queue handles. Distinct families each get their own queue; aliased families share the handle.
        // Call sites use SubmitCompute2/SubmitTransfer2 either way, so the alias is invisible past this point.
        vkGetDeviceQueue(m_Device, m_GraphicsFamily, 0, &m_GraphicsQueue);
        m_ComputeQueue  = m_ComputeIsAsync  ? VK_NULL_HANDLE : m_GraphicsQueue;
        m_TransferQueue = m_TransferIsAsync ? VK_NULL_HANDLE : m_GraphicsQueue;
        if (m_ComputeIsAsync)  vkGetDeviceQueue(m_Device, m_ComputeFamily,  0, &m_ComputeQueue);
        if (m_TransferIsAsync) vkGetDeviceQueue(m_Device, m_TransferFamily, 0, &m_TransferQueue);

        // Deduped family list backs CONCURRENT-sharing resource creation. Already in canonical order: graphics first,
        // then async-compute (if distinct), then async-transfer (if distinct); distinctFamilies was built that way.
        m_ConcurrentFamilyIndices = distinctFamilies;

        LH_LOG(Renderer, info, "Queue layout — graphics={}, compute={} ({}), transfer={} ({})",
            m_GraphicsFamily,
            m_ComputeFamily,  m_ComputeIsAsync  ? "async" : "aliased",
            m_TransferFamily, m_TransferIsAsync ? "async" : "aliased");

        // Command pool for ImmediateSubmit (graphics family: init-time IBL precompute, frame-debugger archive ops).
        VkCommandPoolCreateInfo poolInfo{};
        poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        poolInfo.queueFamilyIndex = m_GraphicsFamily;
        poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;

        vkCreateCommandPool(m_Device, &poolInfo, nullptr, &m_CommandPool);
    }

    void VulkanContext::LoadRayTracingFunctions()
    {
        // RT-mandatory: any missing fp is fatal. Validation-layer rules forbid silent fallback;
        // call sites would dispatch through null and trip the validation layer or crash at use site.
        #define LH_LOAD_RT_FN(field) \
            m_RtFn.field = (PFN_##field)vkGetDeviceProcAddr(m_Device, #field); \
            if (!m_RtFn.field) LH_LOG(Renderer, critical, "RT entry point missing: " #field)

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

    void VulkanContext::LoadCheckpointFunctions()
    {
        m_CheckpointFn.vkCmdSetCheckpointNV =
            (PFN_vkCmdSetCheckpointNV)vkGetDeviceProcAddr(m_Device, "vkCmdSetCheckpointNV");
        m_CheckpointFn.vkGetQueueCheckpointDataNV =
            (PFN_vkGetQueueCheckpointDataNV)vkGetDeviceProcAddr(m_Device, "vkGetQueueCheckpointDataNV");
        if (!m_CheckpointFn.vkCmdSetCheckpointNV || !m_CheckpointFn.vkGetQueueCheckpointDataNV)
        {
            LH_LOG(Renderer, warn, "VK_NV_device_diagnostic_checkpoints fp load failed — disabling");
            m_CheckpointFn = {};
            m_CheckpointsAvailable = false;
        }
    }

    void VulkanContext::LoadDebugUtilsFunctions()
    {
        // VK_EXT_debug_utils procs (enabled with validation); resolve to null when off, call sites null-guard.
        m_DebugUtilsFn.vkSetDebugUtilsObjectNameEXT =
            (PFN_vkSetDebugUtilsObjectNameEXT)vkGetDeviceProcAddr(m_Device, "vkSetDebugUtilsObjectNameEXT");
        m_DebugUtilsFn.vkCmdBeginDebugUtilsLabelEXT =
            (PFN_vkCmdBeginDebugUtilsLabelEXT)vkGetDeviceProcAddr(m_Device, "vkCmdBeginDebugUtilsLabelEXT");
        m_DebugUtilsFn.vkCmdEndDebugUtilsLabelEXT =
            (PFN_vkCmdEndDebugUtilsLabelEXT)vkGetDeviceProcAddr(m_Device, "vkCmdEndDebugUtilsLabelEXT");
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
            LH_LOG(Renderer, error, "VulkanContext: Queue Submit Failed!");
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
        // Capture VkResult: `-4` = VK_ERROR_DEVICE_LOST, `-2` = VK_ERROR_OUT_OF_DEVICE_MEMORY,
        // `-1` = VK_ERROR_OUT_OF_HOST_MEMORY. The bare "Failed!" log dropped this and obscured TDR diagnosis.
        const VkResult r = vkQueueSubmit2(m_GraphicsQueue, 1, &submitInfo, fence);
        if (r != VK_SUCCESS)
        {
            LH_LOG(Renderer, error, "VulkanContext: Graphics SubmitInfo2 failed — VkResult={}", (int)r);
            if (r == VK_ERROR_DEVICE_LOST) DumpCheckpointsOnDeviceLost("Graphics submit");
            return false;
        }
        return true;
    }

    bool VulkanContext::SubmitCompute2(const VkSubmitInfo2& submitInfo, VkFence fence)
    {
        // Aliased compute queue still locks its own mutex; vkQueueSubmit2 is not re-entrant on the same VkQueue
        // even when handles match. Per-mutex prevents contention with concurrent graphics submits.
        std::lock_guard<std::mutex> lock(m_ComputeQueueMutex);
        const VkResult r = vkQueueSubmit2(m_ComputeQueue, 1, &submitInfo, fence);
        if (r != VK_SUCCESS)
        {
            LH_LOG(Renderer, error, "VulkanContext: Compute SubmitInfo2 failed — VkResult={}", (int)r);
            if (r == VK_ERROR_DEVICE_LOST) DumpCheckpointsOnDeviceLost("Compute submit");
            return false;
        }
        return true;
    }

    bool VulkanContext::SubmitTransfer2(const VkSubmitInfo2& submitInfo, VkFence fence)
    {
        std::lock_guard<std::mutex> lock(m_TransferQueueMutex);
        const VkResult r = vkQueueSubmit2(m_TransferQueue, 1, &submitInfo, fence);
        if (r != VK_SUCCESS)
        {
            LH_LOG(Renderer, error, "VulkanContext: Transfer SubmitInfo2 failed — VkResult={}", (int)r);
            if (r == VK_ERROR_DEVICE_LOST) DumpCheckpointsOnDeviceLost("Transfer submit");
            return false;
        }
        return true;
    }

    void VulkanContext::DumpCheckpointsOnDeviceLost(const char* originLabel)
    {
        // Fire-once gate. Once the device is lost, every subsequent submit returns -4 and would
        // re-dump every frame forever. The first dump localized to a specific GPU command is the
        // useful one; further dumps would just be noise.
        static std::atomic<bool> s_Dumped{ false };
        bool expected = false;
        if (!s_Dumped.compare_exchange_strong(expected, true)) return;

        LH_PROFILE_MESSAGE_COLOR(originLabel ? originLabel : "VK_ERROR_DEVICE_LOST", 0xFF4040);

        // Aftermath crash dump first: the richest post-mortem signal (no-op without the SDK). The
        // checkpoint dump below is a fallback only: the markers are often wiped by the GPU reset, so
        // "no checkpoints recorded" is expected, not informative. see arch/gpu-crash-debugging.md
        AftermathCrashTracker::OnDeviceLost();

        LH_LOG(Renderer, critical, "─── GPU device lost (origin: {}) — dumping checkpoints ───", originLabel);
        if (!m_CheckpointsAvailable || !m_CheckpointFn.vkGetQueueCheckpointDataNV)
        {
            LH_LOG(Renderer, critical, "  VK_NV_device_diagnostic_checkpoints unavailable — no localization possible");
            return;
        }

        auto dump = [this](const char* qLabel, VkQueue queue)
        {
            if (queue == VK_NULL_HANDLE) return;
            u32 count = 0;
            m_CheckpointFn.vkGetQueueCheckpointDataNV(queue, &count, nullptr);
            if (count == 0)
            {
                LH_LOG(Renderer, critical, "  [{}] no checkpoints recorded", qLabel);
                return;
            }
            std::vector<VkCheckpointDataNV> data(count, { VK_STRUCTURE_TYPE_CHECKPOINT_DATA_NV });
            m_CheckpointFn.vkGetQueueCheckpointDataNV(queue, &count, data.data());

            LH_LOG(Renderer, critical, "  [{}] {} checkpoint(s) in-flight or just-executed:", qLabel, count);
            for (const auto& cp : data)
            {
                const char* name = GpuCheckpointRegistry::Resolve(cp.pCheckpointMarker);
                LH_LOG(Renderer, critical, "    stage=0x{:08x} marker={}",
                                 (u32)cp.stage, name ? name : "(unknown)");
            }
        };

        dump("Graphics", m_GraphicsQueue);
        if (m_ComputeIsAsync)  dump("Compute",  m_ComputeQueue);
        if (m_TransferIsAsync) dump("Transfer", m_TransferQueue);
        LH_LOG(Renderer, critical, "─── End checkpoint dump ───");
    }

    VkResult VulkanContext::Present(const VkPresentInfoKHR& presentInfo)
    {
        std::lock_guard<std::mutex> lock(m_QueueMutex);
        const VkResult r = vkQueuePresentKHR(m_GraphicsQueue, &presentInfo);
        if (r == VK_ERROR_DEVICE_LOST) DumpCheckpointsOnDeviceLost("Present");  // a TDR can first surface here
        return r;
    }

    void VulkanContext::ApplyConcurrentSharing(VkBufferCreateInfo& info) const
    {
        // Single-family layouts: leave EXCLUSIVE. CONCURRENT with one family index is implementation-defined
        // and validation-noisy; callers expect a clean fallback on iGPU / single-queue hardware.
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
        // Drain under lock, run outside; a deletor may push (nested resource release).
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
