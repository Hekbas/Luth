#pragma once

#include "luth/core/luthTypes.h"
#include "luth/renderer/vulkan/VKDevice.h"

#include <GLFW/glfw3.h>
#include <vector>

namespace Luth
{
    class VKSwapchain
    {
    public:
        struct CreateInfo
        {
            VkPhysicalDevice physicalDevice;
            VkDevice logicalDevice;
            VkSurfaceKHR surface;
            u32 width;
            u32 height;
        };

        VKSwapchain(const CreateInfo& createInfo);
        ~VKSwapchain();

        void Recreate(uint32_t newWidth, uint32_t newHeight);

        VkSwapchainKHR GetHandle() const { return m_Swapchain; }
        VkFormat GetImageFormat() const { return m_SurfaceFormat.format; }
        VkExtent2D GetExtent() const { return m_Extent; }
        const std::vector<VkImageView>& GetImageViews() const { return m_ImageViews; }

    private:
        void CreateSwapchain();
        void CreateImageViews();
        void Cleanup();

        VkSurfaceFormatKHR ChooseSurfaceFormat() const;
        VkPresentModeKHR ChoosePresentMode() const;
        VkExtent2D ChooseExtent(uint32_t width, uint32_t height) const;

        // Vulkan handles (not owned)
        VkPhysicalDevice m_PhysicalDevice;
        VkDevice m_LogicalDevice;
        VkSurfaceKHR m_Surface;

        // Swapchain State
        VkSwapchainKHR m_Swapchain = VK_NULL_HANDLE;
        VkSurfaceFormatKHR m_SurfaceFormat;
        VkPresentModeKHR m_PresentMode;
        VkExtent2D m_Extent;

        std::vector<VkImage> m_Images;
        std::vector<VkImageView> m_ImageViews;
    };
}
