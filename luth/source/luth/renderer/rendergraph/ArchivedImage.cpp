#include "luthpch.h"
#include "luth/renderer/rendergraph/ArchivedImage.h"
#include "luth/renderer/backend/vulkan/VulkanAllocator.h"

#include <vma/vk_mem_alloc.h>
#include <backends/imgui_impl_vulkan.h>

namespace Luth::RG
{
    VkImageView ArchivedImage::GetOrCreateLayerView(VkDevice device, u32 layer)
    {
        if (layer >= layers || image == VK_NULL_HANDLE) return VK_NULL_HANDLE;
        if (layerViews.size() < layers) layerViews.resize(layers, VK_NULL_HANDLE);
        if (layerViews[layer] != VK_NULL_HANDLE) return layerViews[layer];

        VkImageViewCreateInfo vci{ VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
        vci.image    = image;
        vci.viewType = VK_IMAGE_VIEW_TYPE_2D;  // single layer regardless of source array
        vci.format   = format;
        vci.subresourceRange.aspectMask     = isDepth ? VK_IMAGE_ASPECT_DEPTH_BIT
                                                       : VK_IMAGE_ASPECT_COLOR_BIT;
        vci.subresourceRange.baseMipLevel   = 0;
        vci.subresourceRange.levelCount     = 1;
        vci.subresourceRange.baseArrayLayer = layer;
        vci.subresourceRange.layerCount     = 1;

        vkCreateImageView(device, &vci, nullptr, &layerViews[layer]);
        return layerViews[layer];
    }

    void ArchivedImage::Destroy(VkDevice device, VmaAllocator allocator)
    {
        if (imguiDescSet != VK_NULL_HANDLE)
        {
            ImGui_ImplVulkan_RemoveTexture(imguiDescSet);
            imguiDescSet = VK_NULL_HANDLE;
        }

        for (VkImageView lv : layerViews)
        {
            if (lv != VK_NULL_HANDLE) vkDestroyImageView(device, lv, nullptr);
        }
        layerViews.clear();

        if (view != VK_NULL_HANDLE)
        {
            vkDestroyImageView(device, view, nullptr);
            view = VK_NULL_HANDLE;
        }

        if (image != VK_NULL_HANDLE && alloc != nullptr)
        {
            // Route through VulkanAllocator so MemoryTracker.RecordFree fires
            // and the editor's GPU memory counter stays balanced.
            (void)allocator; // VulkanAllocator owns the singleton allocator
            VulkanAllocator::FreeImage(image, alloc);
            image = VK_NULL_HANDLE;
            alloc = nullptr;
        }
    }
}
