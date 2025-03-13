#include "Luthpch.h"
#include "luth/renderer/Renderer.h"
#include "luth/renderer/Buffer.h"
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
        CreateSwapchain();
        CreateRenderPass();
        CreateGraphicsPipeline();
        CreateFramebuffers();
        CreateCommandPool();
        CreateCommandBuffers();
        CreateSyncObjects();

        LH_CORE_INFO("Vulkan renderer initialization complete");
    }

    void VKRendererAPI::Shutdown()
    {
        vkDeviceWaitIdle(m_LogicalDevice->GetHandle());

        m_Sync.reset();

        if (!m_CommandBuffers.empty()) {
            vkFreeCommandBuffers(m_LogicalDevice->GetHandle(),
                m_CommandPool->GetHandle(),
                static_cast<uint32_t>(m_CommandBuffers.size()),
                m_CommandBuffers.data());
            m_CommandBuffers.clear();
        }

        m_CommandPool.reset();
        m_Framebuffers.clear();
        m_GraphicsPipeline.reset();
        m_RenderPass.reset();
        m_Swapchain.reset();
        m_LogicalDevice.reset();
        m_PhysicalDevice.reset();

        if (m_Surface) {
            vkDestroySurfaceKHR(m_Instance, m_Surface, nullptr);
            m_Surface = VK_NULL_HANDLE;
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

    void VKRendererAPI::SubmitMesh(const std::shared_ptr<Mesh>& mesh)
    {
        m_CurrentMesh = std::dynamic_pointer_cast<VKMesh>(mesh);
    }

    void VKRendererAPI::DrawIndexed(u32 count) {}

    void VKRendererAPI::RecordCommandBuffer(VkCommandBuffer commandBuffer, uint32_t imageIndex)
    {
        VkCommandBufferBeginInfo beginInfo{};
        beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;

        VK_CHECK_RESULT(vkBeginCommandBuffer(commandBuffer, &beginInfo),
            "Failed to begin recording command buffer!");

        VkRenderPassBeginInfo renderPassInfo{};
        renderPassInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
        renderPassInfo.renderPass = m_RenderPass->GetHandle();
        renderPassInfo.framebuffer = m_Framebuffers[imageIndex]->GetHandle();
        renderPassInfo.renderArea.offset = { 0, 0 };
        renderPassInfo.renderArea.extent = m_Swapchain->GetExtent();

        VkClearValue clearColor = { {{0.0f, 0.0f, 0.0f, 1.0f}} };
        renderPassInfo.clearValueCount = 1;
        renderPassInfo.pClearValues = &clearColor;

        vkCmdBeginRenderPass(commandBuffer, &renderPassInfo, VK_SUBPASS_CONTENTS_INLINE);
        vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, m_GraphicsPipeline->GetHandle());

        if (m_CurrentMesh) {
            auto vkMesh = std::static_pointer_cast<VKMesh>(m_CurrentMesh);

            // Bind vertex buffer
            VkBuffer vertexBuffers[] = { vkMesh->GetVertexBuffer() };
            VkDeviceSize offsets[] = { 0 };
            vkCmdBindVertexBuffers(commandBuffer, 0, 1, vertexBuffers, offsets);

            // Draw command
            if (vkMesh->GetIndexCount() > 0) {
                vkCmdBindIndexBuffer(commandBuffer, vkMesh->GetIndexBuffer(),
                    0, VK_INDEX_TYPE_UINT32);
                vkCmdDrawIndexed(commandBuffer, vkMesh->GetIndexCount(), 1, 0, 0, 0);
            }
            else {
                vkCmdDraw(commandBuffer, vkMesh->GetVertexCount(), 1, 0, 0);
            }
        }

        //vkCmdDraw(commandBuffer, 3, 1, 0, 0); // Draw triangle
        vkCmdEndRenderPass(commandBuffer);

        VK_CHECK_RESULT(vkEndCommandBuffer(commandBuffer),
            "Failed to record command buffer!");
    }

    void VKRendererAPI::DrawFrame()
    {
        auto& frame = m_Sync->GetCurrentFrame();

        // Wait for previous frame
        vkWaitForFences(m_LogicalDevice->GetHandle(), 1, &frame.inFlightFence, VK_TRUE, UINT64_MAX);
        vkResetFences(m_LogicalDevice->GetHandle(), 1, &frame.inFlightFence);

        // Acquire next image
        uint32_t imageIndex;
        VkResult result = vkAcquireNextImageKHR(
            m_LogicalDevice->GetHandle(),
            m_Swapchain->GetHandle(),
            UINT64_MAX,
            frame.imageAvailable,
            VK_NULL_HANDLE,
            &imageIndex
        );

        if (result == VK_ERROR_OUT_OF_DATE_KHR) {
            RecreateSwapchain();
            return;
        }

        // Record command buffer
        VkCommandBuffer commandBuffer = m_CommandBuffers[imageIndex];
        vkResetCommandBuffer(commandBuffer, 0);
        RecordCommandBuffer(commandBuffer, imageIndex);

        // Submit commands
        VkSubmitInfo submitInfo{};
        submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;

        VkSemaphore waitSemaphores[] = { frame.imageAvailable };
        VkPipelineStageFlags waitStages[] = { VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT };
        submitInfo.waitSemaphoreCount = 1;
        submitInfo.pWaitSemaphores = waitSemaphores;
        submitInfo.pWaitDstStageMask = waitStages;
        submitInfo.commandBufferCount = 1;
        submitInfo.pCommandBuffers = &commandBuffer;

        VkSemaphore signalSemaphores[] = { frame.renderFinished };
        submitInfo.signalSemaphoreCount = 1;
        submitInfo.pSignalSemaphores = signalSemaphores;

        VK_CHECK_RESULT(vkQueueSubmit(
            m_LogicalDevice->GetGraphicsQueue(),
            1, &submitInfo,
            frame.inFlightFence
        ), "Failed to submit draw command buffer!");

        // Present
        VkPresentInfoKHR presentInfo{};
        presentInfo.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
        presentInfo.waitSemaphoreCount = 1;
        presentInfo.pWaitSemaphores = signalSemaphores;

        VkSwapchainKHR swapchains[] = { m_Swapchain->GetHandle() };
        presentInfo.swapchainCount = 1;
        presentInfo.pSwapchains = swapchains;
        presentInfo.pImageIndices = &imageIndex;

        result = vkQueuePresentKHR(m_LogicalDevice->GetPresentQueue(), &presentInfo);

        if (result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR) {
            RecreateSwapchain();
        }
        else {
            m_Sync->AdvanceFrame();
        }
    }


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

        QueueFamilyIndices indices = m_PhysicalDevice->FindQueueFamilies(m_Surface);

        m_LogicalDevice = std::make_unique<VKLogicalDevice>(
            m_PhysicalDevice->GetHandle(),
            indices,
            std::vector({ VK_KHR_SWAPCHAIN_EXTENSION_NAME })
        );
    }

    void VKRendererAPI::CreateSwapchain()
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
        
        LH_CORE_INFO("Created Vulkan swapchain with {0} images",
            m_Swapchain->GetImageViews().size());
    }

    void VKRendererAPI::CreateRenderPass()
    {
        if (m_RenderPass) {
            m_RenderPass.reset();
        }

        VkFormat swapchainFormat = m_Swapchain->GetImageFormat();
        m_RenderPass = std::make_unique<VKRenderPass>(
            m_LogicalDevice->GetHandle(),
            swapchainFormat
        );

        LH_CORE_INFO("Created Vulkan render pass with format: {0}", (int)swapchainFormat);
    }

    void VKRendererAPI::CreateGraphicsPipeline()
    {
        if (m_GraphicsPipeline) {
            m_GraphicsPipeline.reset();
        }

        // Vertex layout expected by the shader
        BufferLayout vertexLayout = {
            { ShaderDataType::Float2, "inPosition" },
            { ShaderDataType::Float3, "inColor" }
        };
        
        VkExtent2D swapchainExtent = m_Swapchain->GetExtent();
        VkRenderPass renderPass = m_RenderPass->GetHandle();
        auto bindingDesc = vertexLayout.GetBindingDescriptions();
        auto attributeDesc = vertexLayout.GetAttributeDescriptions();

        m_GraphicsPipeline = std::make_unique<VKGraphicsPipeline>(
            m_LogicalDevice->GetHandle(),
            swapchainExtent,
            renderPass,
            bindingDesc,
            attributeDesc
        );

        LH_CORE_INFO("Created Vulkan graphics pipeline with extent: {0}x{1}",
            swapchainExtent.width, swapchainExtent.height);
    }

    void VKRendererAPI::CreateFramebuffers()
    {
        m_Framebuffers.clear();
        auto& imageViews = m_Swapchain->GetImageViews();
        for (auto imageView : imageViews) {
            m_Framebuffers.emplace_back(std::make_unique<VKFramebuffer>(
                m_LogicalDevice->GetHandle(),
                m_RenderPass->GetHandle(),
                imageView,
                m_Swapchain->GetExtent()
            ));
        }
    }

    void VKRendererAPI::CreateCommandPool()
    {
        QueueFamilyIndices queueFamilyIndices = m_PhysicalDevice->FindQueueFamilies(m_Surface);

        m_CommandPool = std::make_unique<VKCommandPool>(
            m_LogicalDevice->GetHandle(),
            queueFamilyIndices.graphicsFamily.value()
        );
        LH_CORE_INFO("Created Vulkan command pool");
    }

    void VKRendererAPI::CreateCommandBuffers()
    {
        m_CommandBuffers.resize(m_Framebuffers.size());

        VkCommandBufferAllocateInfo allocInfo{};
        allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        allocInfo.commandPool = m_CommandPool->GetHandle();
        allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        allocInfo.commandBufferCount = (uint32_t)m_CommandBuffers.size();

        VK_CHECK_RESULT(vkAllocateCommandBuffers(m_LogicalDevice->GetHandle(), &allocInfo, m_CommandBuffers.data()),
            "Failed to allocate command buffers!");
    }

    void VKRendererAPI::CreateSyncObjects()
    {
        m_Sync = std::make_unique<VKSync>(
            m_LogicalDevice->GetHandle(),
            MAX_FRAMES_IN_FLIGHT
        );
        LH_CORE_INFO("Created Vulkan synchronization objects");
    }

    void VKRendererAPI::RecreateSwapchain()
    {
        vkDeviceWaitIdle(m_LogicalDevice->GetHandle());

        // Proper cleanup order
        m_CommandBuffers.clear();
        m_Framebuffers.clear();
        m_GraphicsPipeline.reset();
        m_RenderPass.reset();
        m_Swapchain.reset();

        // Proper recreation order
        CreateSwapchain();
        CreateRenderPass();
        CreateGraphicsPipeline();
        CreateFramebuffers();
        CreateCommandBuffers();
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
}
