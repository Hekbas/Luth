#include "luthpch.h"
#include "luth/renderer/rendergraph/ArchivedImage.h"
#include "luth/renderer/backend/vulkan/VulkanAllocator.h"

#include <vma/vk_mem_alloc.h>
#include <backends/imgui_impl_vulkan.h>

namespace Luth::RG
{
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
