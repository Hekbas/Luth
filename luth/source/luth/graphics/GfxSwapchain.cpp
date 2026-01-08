#include "luthpch.h"
#include "luth/graphics/GfxSwapchain.h"
#include "luth/core/Log.h"
#include <algorithm>

namespace Luth::Gfx
{
    GfxSwapchain::GfxSwapchain(u32 width, u32 height)
        : m_Width(width), m_Height(height)
    {
        Create();
    }

    GfxSwapchain::~GfxSwapchain()
    {
        Cleanup();
    }

    void GfxSwapchain::Resize(u32 width, u32 height)
    {
        m_Width = width;
        m_Height = height;
        Create();
    }

    void GfxSwapchain::Create()
    {
        // Cleanup old swapchain if exists (recreation)
        VkSwapchainKHR oldSwapchain = m_Swapchain;
        if (oldSwapchain)
        {
            // We must wait for device idle before destroying old swapchain resources
            // In a real engine, we might keep the old one alive until retired, 
            // but for simplicity we wait.
            vkDeviceWaitIdle(GfxContext::Get().GetDevice());
            Cleanup();
        }

        auto& ctx = GfxContext::Get();
        VkSurfaceCapabilitiesKHR capabilities;
        vkGetPhysicalDeviceSurfaceCapabilitiesKHR(ctx.GetPhysicalDevice(), ctx.GetSurface(), &capabilities);

        VkSurfaceFormatKHR surfaceFormat = ChooseSurfaceFormat();
        VkPresentModeKHR presentMode = ChoosePresentMode();
        VkExtent2D extent = ChooseExtent(m_Width, m_Height);

        u32 imageCount = capabilities.minImageCount + 1;
        if (capabilities.maxImageCount > 0 && imageCount > capabilities.maxImageCount)
            imageCount = capabilities.maxImageCount;

        VkSwapchainCreateInfoKHR createInfo{};
        createInfo.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
        createInfo.surface = ctx.GetSurface();
        createInfo.minImageCount = imageCount;
        createInfo.imageFormat = surfaceFormat.format;
        createInfo.imageColorSpace = surfaceFormat.colorSpace;
        createInfo.imageExtent = extent;
        createInfo.imageArrayLayers = 1;
        
        // Important: Add TRANSFER_DST for clearing/copying
        createInfo.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;

        // Queue Family Sharing
        u32 queueFamilyIndices[] = { ctx.GetGraphicsQueue().familyIndex, ctx.GetPresentQueue().familyIndex };
        if (ctx.GetGraphicsQueue().familyIndex != ctx.GetPresentQueue().familyIndex)
        {
            createInfo.imageSharingMode = VK_SHARING_MODE_CONCURRENT;
            createInfo.queueFamilyIndexCount = 2;
            createInfo.pQueueFamilyIndices = queueFamilyIndices;
        }
        else
        {
            createInfo.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
        }

        createInfo.preTransform = capabilities.currentTransform;
        createInfo.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
        createInfo.presentMode = presentMode;
        createInfo.clipped = VK_TRUE;
        createInfo.oldSwapchain = oldSwapchain; // Chain for smooth resize

        if (vkCreateSwapchainKHR(ctx.GetDevice(), &createInfo, nullptr, &m_Swapchain) != VK_SUCCESS)
        {
            LH_CORE_CRITICAL("Failed to create Swapchain!");
        }

        // Get Images
        vkGetSwapchainImagesKHR(ctx.GetDevice(), m_Swapchain, &imageCount, nullptr);
        m_Images.resize(imageCount);
        vkGetSwapchainImagesKHR(ctx.GetDevice(), m_Swapchain, &imageCount, m_Images.data());

        m_Format = surfaceFormat.format;
        m_Extent = extent;

        // Create Views
        m_ImageViews.resize(imageCount);
        for (size_t i = 0; i < imageCount; i++)
        {
            VkImageViewCreateInfo viewInfo{};
            viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
            viewInfo.image = m_Images[i];
            viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
            viewInfo.format = m_Format;
            viewInfo.components.r = VK_COMPONENT_SWIZZLE_IDENTITY;
            viewInfo.components.g = VK_COMPONENT_SWIZZLE_IDENTITY;
            viewInfo.components.b = VK_COMPONENT_SWIZZLE_IDENTITY;
            viewInfo.components.a = VK_COMPONENT_SWIZZLE_IDENTITY;
            viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            viewInfo.subresourceRange.baseMipLevel = 0;
            viewInfo.subresourceRange.levelCount = 1;
            viewInfo.subresourceRange.baseArrayLayer = 0;
            viewInfo.subresourceRange.layerCount = 1;

            if (vkCreateImageView(ctx.GetDevice(), &viewInfo, nullptr, &m_ImageViews[i]) != VK_SUCCESS)
            {
                LH_CORE_CRITICAL("Failed to create Swapchain Image View!");
            }
        }
    }

    void GfxSwapchain::Cleanup()
    {
        auto device = GfxContext::Get().GetDevice();
        for (auto view : m_ImageViews)
            vkDestroyImageView(device, view, nullptr);
        
        m_ImageViews.clear();

        if (m_Swapchain)
        {
            vkDestroySwapchainKHR(device, m_Swapchain, nullptr);
            m_Swapchain = VK_NULL_HANDLE;
        }
    }

    bool GfxSwapchain::AcquireNextImage(VkSemaphore signalSemaphore, u32& outImageIndex)
    {
        VkResult result = vkAcquireNextImageKHR(
            GfxContext::Get().GetDevice(),
            m_Swapchain,
            UINT64_MAX,
            signalSemaphore,
            VK_NULL_HANDLE,
            &outImageIndex
        );

        if (result == VK_ERROR_OUT_OF_DATE_KHR)
        {
            return false;
        }
        else if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR)
        {
            LH_CORE_ERROR("Failed to acquire swapchain image!");
            return false;
        }
        return true;
    }

    bool GfxSwapchain::Present(VkSemaphore waitSemaphore, u32 imageIndex)
    {
        VkPresentInfoKHR presentInfo{};
        presentInfo.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
        presentInfo.waitSemaphoreCount = 1;
        presentInfo.pWaitSemaphores = &waitSemaphore;
        
        VkSwapchainKHR swapchains[] = { m_Swapchain };
        presentInfo.swapchainCount = 1;
        presentInfo.pSwapchains = swapchains;
        presentInfo.pImageIndices = &imageIndex;

        VkResult result = vkQueuePresentKHR(GfxContext::Get().GetPresentQueue().handle, &presentInfo);

        if (result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR)
        {
            return false;
        }
        else if (result != VK_SUCCESS)
        {
            LH_CORE_ERROR("Failed to present swapchain image!");
            return false;
        }
        return true;
    }

    VkSurfaceFormatKHR GfxSwapchain::ChooseSurfaceFormat()
    {
        auto& ctx = GfxContext::Get();
        u32 count;
        vkGetPhysicalDeviceSurfaceFormatsKHR(ctx.GetPhysicalDevice(), ctx.GetSurface(), &count, nullptr);
        std::vector<VkSurfaceFormatKHR> formats(count);
        vkGetPhysicalDeviceSurfaceFormatsKHR(ctx.GetPhysicalDevice(), ctx.GetSurface(), &count, formats.data());

        for (const auto& format : formats)
        {
            if (format.format == VK_FORMAT_B8G8R8A8_SRGB && format.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR)
                return format;
        }
        return formats[0];
    }

    VkPresentModeKHR GfxSwapchain::ChoosePresentMode()
    {
        auto& ctx = GfxContext::Get();
        u32 count;
        vkGetPhysicalDeviceSurfacePresentModesKHR(ctx.GetPhysicalDevice(), ctx.GetSurface(), &count, nullptr);
        std::vector<VkPresentModeKHR> modes(count);
        vkGetPhysicalDeviceSurfacePresentModesKHR(ctx.GetPhysicalDevice(), ctx.GetSurface(), &count, modes.data());

        for (const auto& mode : modes)
        {
            if (mode == VK_PRESENT_MODE_MAILBOX_KHR) return mode;
        }
        return VK_PRESENT_MODE_FIFO_KHR;
    }

    VkExtent2D GfxSwapchain::ChooseExtent(u32 width, u32 height)
    {
        auto& ctx = GfxContext::Get();
        VkSurfaceCapabilitiesKHR capabilities;
        vkGetPhysicalDeviceSurfaceCapabilitiesKHR(ctx.GetPhysicalDevice(), ctx.GetSurface(), &capabilities);

        if (capabilities.currentExtent.width != UINT32_MAX)
            return capabilities.currentExtent;

        VkExtent2D actualExtent = {
            std::clamp(width, capabilities.minImageExtent.width, capabilities.maxImageExtent.width),
            std::clamp(height, capabilities.minImageExtent.height, capabilities.maxImageExtent.height)
        };
        return actualExtent;
    }
}
