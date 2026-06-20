#include "luthpch.h"
#include <chrono>
#include <atomic>
#include "VulkanSwapchain.h"
#include "VulkanContext.h"
#include "luth/core/diagnostics/Log.h"

#define GLFW_EXPOSE_NATIVE_WIN32
#include <GLFW/glfw3.h>
#include <GLFW/glfw3native.h>

namespace Luth
{
    VulkanSwapchain::VulkanSwapchain(void* windowHandle)
        : m_WindowHandle(windowHandle)
    {
    }

    VulkanSwapchain::~VulkanSwapchain()
    {
        Cleanup();
        vkDestroySurfaceKHR(VulkanContext::Get().GetInstance(), m_Surface, nullptr);
    }

    void VulkanSwapchain::Init()
    {
        CreateSurface();

        // Block on (0,0) extent (launched-minimized) — Vulkan rejects zero-sized swapchains.
        int w = 0, h = 0;
        while (w == 0 || h == 0)
        {
            glfwGetWindowSize((GLFWwindow*)m_WindowHandle, &w, &h);
            if (w == 0 || h == 0) glfwWaitEvents();
        }
        CreateSwapchain((u32)w, (u32)h);
        CreateImageViews();
    }

    void VulkanSwapchain::Cleanup()
    {
        VkDevice device = VulkanContext::Get().GetDevice();
        
        for (auto imageView : m_ImageViews) {
            vkDestroyImageView(device, imageView, nullptr);
        }
        m_ImageViews.clear();

        if (m_Swapchain != VK_NULL_HANDLE) {
            vkDestroySwapchainKHR(device, m_Swapchain, nullptr);
            m_Swapchain = VK_NULL_HANDLE;
        }
    }

    void VulkanSwapchain::Recreate(u32 width, u32 height)
    {
        LH_PROFILE_MESSAGE(("Swapchain recreate " + std::to_string(width) + "x" + std::to_string(height)).c_str());
        vkDeviceWaitIdle(VulkanContext::Get().GetDevice());
        Cleanup();
        CreateSwapchain(width, height);
        CreateImageViews();
    }

    void VulkanSwapchain::CreateSurface()
    {
        if (glfwCreateWindowSurface(VulkanContext::Get().GetInstance(), (GLFWwindow*)m_WindowHandle, nullptr, &m_Surface) != VK_SUCCESS) {
            LH_CORE_CRITICAL("Failed to create window surface!");
        }

        // Surface presentation support is not spec-guaranteed (true on desktop GPUs in practice).
        auto& ctx = VulkanContext::Get();
        VkBool32 presentSupport = VK_FALSE;
        vkGetPhysicalDeviceSurfaceSupportKHR(ctx.GetPhysicalDevice(), ctx.GetGraphicsFamily(), m_Surface, &presentSupport);
        if (!presentSupport)
            LH_CORE_CRITICAL("Graphics queue family does not support presentation on this surface");
    }

    void VulkanSwapchain::CreateSwapchain(u32 width, u32 height)
    {
        auto& ctx = VulkanContext::Get();
        VkPhysicalDevice physicalDevice = ctx.GetPhysicalDevice();
        VkDevice device = ctx.GetDevice();

        // Capabilities
        VkSurfaceCapabilitiesKHR capabilities;
        vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physicalDevice, m_Surface, &capabilities);

        // Formats
        uint32_t formatCount;
        vkGetPhysicalDeviceSurfaceFormatsKHR(physicalDevice, m_Surface, &formatCount, nullptr);
        std::vector<VkSurfaceFormatKHR> formats(formatCount);
        vkGetPhysicalDeviceSurfaceFormatsKHR(physicalDevice, m_Surface, &formatCount, formats.data());

        // Present Modes
        uint32_t presentModeCount;
        vkGetPhysicalDeviceSurfacePresentModesKHR(physicalDevice, m_Surface, &presentModeCount, nullptr);
        std::vector<VkPresentModeKHR> presentModes(presentModeCount);
        vkGetPhysicalDeviceSurfacePresentModesKHR(physicalDevice, m_Surface, &presentModeCount, presentModes.data());

        // Choose Format (Prefer B8G8R8A8 UNORM to avoid double gamma correction on UI)
        VkSurfaceFormatKHR surfaceFormat = formats[0];
        for (const auto& availableFormat : formats) {
            if (availableFormat.format == VK_FORMAT_B8G8R8A8_UNORM && 
                availableFormat.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
                surfaceFormat = availableFormat;
                break;
            }
        }
        m_ImageFormat = surfaceFormat.format;

        // Choose Present Mode (Prefer Mailbox)
        VkPresentModeKHR presentMode = VK_PRESENT_MODE_FIFO_KHR;
        for (const auto& availablePresentMode : presentModes) {
            if (availablePresentMode == VK_PRESENT_MODE_MAILBOX_KHR) {
                presentMode = availablePresentMode;
                break;
            }
        }

        // Extent
        if (capabilities.currentExtent.width != UINT32_MAX) {
            m_Extent = capabilities.currentExtent;
        } else {
            VkExtent2D actualExtent = { width, height };
            actualExtent.width = std::clamp(actualExtent.width, capabilities.minImageExtent.width, capabilities.maxImageExtent.width);
            actualExtent.height = std::clamp(actualExtent.height, capabilities.minImageExtent.height, capabilities.maxImageExtent.height);
            m_Extent = actualExtent;
        }

        // Image Count
        uint32_t imageCount = capabilities.minImageCount + 1;
        if (capabilities.maxImageCount > 0 && imageCount > capabilities.maxImageCount) {
            imageCount = capabilities.maxImageCount;
        }

        VkSwapchainCreateInfoKHR createInfo{};
        createInfo.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
        createInfo.surface = m_Surface;
        createInfo.minImageCount = imageCount;
        createInfo.imageFormat = surfaceFormat.format;
        createInfo.imageColorSpace = surfaceFormat.colorSpace;
        createInfo.imageExtent = m_Extent;
        createInfo.imageArrayLayers = 1;
        createInfo.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT; // Added TransferDst for blitting/clearing
        createInfo.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
        createInfo.preTransform = capabilities.currentTransform;
        createInfo.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
        createInfo.presentMode = presentMode;
        createInfo.clipped = VK_TRUE;
        createInfo.oldSwapchain = VK_NULL_HANDLE;

        if (vkCreateSwapchainKHR(device, &createInfo, nullptr, &m_Swapchain) != VK_SUCCESS) {
            LH_CORE_CRITICAL("Failed to create swapchain!");
        }

        vkGetSwapchainImagesKHR(device, m_Swapchain, &imageCount, nullptr);
        m_Images.resize(imageCount);
        vkGetSwapchainImagesKHR(device, m_Swapchain, &imageCount, m_Images.data());
    }

    void VulkanSwapchain::CreateImageViews()
    {
        m_ImageViews.resize(m_Images.size());

        for (size_t i = 0; i < m_Images.size(); i++) {
            VkImageViewCreateInfo createInfo{};
            createInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
            createInfo.image = m_Images[i];
            createInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
            createInfo.format = m_ImageFormat;
            createInfo.components.r = VK_COMPONENT_SWIZZLE_IDENTITY;
            createInfo.components.g = VK_COMPONENT_SWIZZLE_IDENTITY;
            createInfo.components.b = VK_COMPONENT_SWIZZLE_IDENTITY;
            createInfo.components.a = VK_COMPONENT_SWIZZLE_IDENTITY;
            createInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            createInfo.subresourceRange.baseMipLevel = 0;
            createInfo.subresourceRange.levelCount = 1;
            createInfo.subresourceRange.baseArrayLayer = 0;
            createInfo.subresourceRange.layerCount = 1;

            if (vkCreateImageView(VulkanContext::Get().GetDevice(), &createInfo, nullptr, &m_ImageViews[i]) != VK_SUCCESS) {
                LH_CORE_CRITICAL("Failed to create image views!");
            }
        }
    }

    // Perf observability: CPU time in the last acquire / present (present-bound signal).
    static std::atomic<f64> s_LastAcquireMs{ 0.0 };
    static std::atomic<f64> s_LastPresentMs{ 0.0 };
    f64 VulkanSwapchain::GetLastAcquireMs() { return s_LastAcquireMs.load(std::memory_order_relaxed); }
    f64 VulkanSwapchain::GetLastPresentMs() { return s_LastPresentMs.load(std::memory_order_relaxed); }

    u32 VulkanSwapchain::AcquireNextImage(VkSemaphore signalSemaphore)
    {
        LH_PROFILE_FUNCTION();

        // Consume Present's deferred rebuild — Acquire runs on main thread (V2), blocking on Recreate is correct here.
        if (m_NeedsRebuild)
        {
            int w = 0, h = 0;
            glfwGetWindowSize((GLFWwindow*)m_WindowHandle, &w, &h);
            if (w > 0 && h > 0) Recreate((u32)w, (u32)h);
            m_NeedsRebuild = false;
            return UINT32_MAX;
        }

        uint32_t imageIndex = 0;
        const auto acqStart = std::chrono::high_resolution_clock::now();
        VkResult result = vkAcquireNextImageKHR(VulkanContext::Get().GetDevice(), m_Swapchain, UINT64_MAX, signalSemaphore, VK_NULL_HANDLE, &imageIndex);
        s_LastAcquireMs.store(std::chrono::duration<f64, std::milli>(std::chrono::high_resolution_clock::now() - acqStart).count(), std::memory_order_relaxed);

        if (result == VK_ERROR_OUT_OF_DATE_KHR)
        {
            int w = 0, h = 0;
            glfwGetWindowSize((GLFWwindow*)m_WindowHandle, &w, &h);
            if (w > 0 && h > 0) Recreate((u32)w, (u32)h);
            return UINT32_MAX;
        }
        if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR)
        {
            LH_CORE_ERROR("Failed to acquire swap chain image (VkResult={})", (int)result);
            return UINT32_MAX;
        }

        m_CurrentFrameIndex = imageIndex;
        return imageIndex;
    }

    VkResult VulkanSwapchain::Present(VkSemaphore waitSemaphore)
    {
        LH_PROFILE_FUNCTION();

        VkPresentInfoKHR presentInfo{};
        presentInfo.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
        presentInfo.waitSemaphoreCount = 1;
        presentInfo.pWaitSemaphores = &waitSemaphore;

        VkSwapchainKHR swapChains[] = { m_Swapchain };
        presentInfo.swapchainCount = 1;
        presentInfo.pSwapchains = swapChains;
        presentInfo.pImageIndices = &m_CurrentFrameIndex;

        const auto presStart = std::chrono::high_resolution_clock::now();
        VkResult result = VulkanContext::Get().Present(presentInfo);
        s_LastPresentMs.store(std::chrono::duration<f64, std::milli>(std::chrono::high_resolution_clock::now() - presStart).count(), std::memory_order_relaxed);

        // Defer rebuild — Present runs on the render fiber; vkDeviceWaitIdle would stall the worker (V2).
        // Acquire (main thread) consumes the flag. SUBOPTIMAL is benign (cosmetic scaling).
        if (result == VK_ERROR_OUT_OF_DATE_KHR)
        {
            m_NeedsRebuild = true;
        }
        else if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR)
        {
            LH_CORE_ERROR("Failed to present swapchain image (VkResult={})", (int)result);
        }

        return result;
    }
}
