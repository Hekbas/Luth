#pragma once

#include "luth/core/types/LuthTypes.h"
#include <vulkan/vulkan.h>
#include <vector>

namespace Luth
{
    class VulkanSwapchain
    {
    public:
        VulkanSwapchain(void* windowHandle);
        ~VulkanSwapchain();

        void Init();
        void Recreate(u32 width, u32 height);
        void Cleanup();

        // Returns the swapchain image index, or UINT32_MAX on failure.
        // Self-rebuilds the swapchain on VK_ERROR_OUT_OF_DATE_KHR before
        // returning the sentinel — caller skips the frame and retries next.
        u32 AcquireNextImage(VkSemaphore signalSemaphore);

        // Presents the image. Returns the underlying VkResult so the caller
        // can react to OUT_OF_DATE / SUBOPTIMAL. Internally triggers a rebuild
        // on out-of-date / suboptimal so the next acquire sees a fresh chain.
        VkResult Present(VkSemaphore waitSemaphore);

        VkFormat GetImageFormat() const { return m_ImageFormat; }
        VkExtent2D GetExtent() const { return m_Extent; }
        u32 GetImageCount() const { return (u32)m_Images.size(); }
        
        VkImageView GetImageView(u32 index) const { return m_ImageViews[index]; }
        VkImage GetImage(u32 index) const { return m_Images[index]; }

        u32 GetCurrentFrameIndex() const { return m_CurrentFrameIndex; }

    private:
        void CreateSurface();
        void CreateSwapchain(u32 width, u32 height);
        void CreateImageViews();

        void* m_WindowHandle;
        VkSurfaceKHR m_Surface = VK_NULL_HANDLE;
        VkSwapchainKHR m_Swapchain = VK_NULL_HANDLE;

        VkFormat m_ImageFormat;
        VkExtent2D m_Extent;

        std::vector<VkImage> m_Images;
        std::vector<VkImageView> m_ImageViews;

        u32 m_CurrentFrameIndex = 0; // Index of the image currently being rendered to

        // Set by Present on OUT_OF_DATE; consumed at the top of the NEXT
        // AcquireNextImage so the rebuild (which calls vkDeviceWaitIdle and
        // would otherwise block the render fiber's worker thread) runs on
        // the main thread instead.
        bool m_NeedsRebuild = false;
    };
}
