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

        // Returns index of the image to render to
        u32 AcquireNextImage(VkSemaphore signalSemaphore);
        
        // Presents the image
        void Present(VkSemaphore waitSemaphore);

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
    };
}
