#pragma once

#include "luth/core/LuthTypes.h"
#include "luth/graphics/GfxContext.h"
#include <vulkan/vulkan.h>
#include <vector>

namespace Luth::Gfx
{
    class GfxSwapchain
    {
    public:
        GfxSwapchain(u32 width, u32 height);
        ~GfxSwapchain();

        void Resize(u32 width, u32 height);

        // Returns true if successful, false if swapchain needs resize
        bool AcquireNextImage(VkSemaphore signalSemaphore, u32& outImageIndex);
        
        // Returns true if successful
        bool Present(VkSemaphore waitSemaphore, u32 imageIndex);

        VkFormat GetFormat() const { return m_Format; }
        VkExtent2D GetExtent() const { return m_Extent; }
        u32 GetImageCount() const { return (u32)m_Images.size(); }
        
        VkImage GetImage(u32 index) const { return m_Images[index]; }
        VkImageView GetImageView(u32 index) const { return m_ImageViews[index]; }

    private:
        void Create();
        void Cleanup();

        VkSurfaceFormatKHR ChooseSurfaceFormat();
        VkPresentModeKHR ChoosePresentMode();
        VkExtent2D ChooseExtent(u32 width, u32 height);

        VkSwapchainKHR m_Swapchain = VK_NULL_HANDLE;
        VkFormat m_Format;
        VkExtent2D m_Extent;
        
        std::vector<VkImage> m_Images;
        std::vector<VkImageView> m_ImageViews;

        u32 m_Width = 0;
        u32 m_Height = 0;
    };
}
