#pragma once

#include "luth/core/LuthTypes.h"

#include <vulkan/vulkan.h>
#include <string>
#include <vector>

typedef struct VmaAllocator_T* VmaAllocator;
typedef struct VmaAllocation_T* VmaAllocation;

namespace Luth::RG
{
    // Persistent staging copy of a render target captured at a point in the frame.
    // Owned and managed by FrameDebugger / IArchiveSink implementations.
    // Lives only while the captured frame is Frozen; freed on next capture or exit.
    struct ArchivedImage
    {
        std::string name;

        VkImage         image  = VK_NULL_HANDLE;
        VkImageView     view   = VK_NULL_HANDLE;        // Whole-image view (all layers, all mips)
        VmaAllocation   alloc  = nullptr;

        u32 width  = 0;
        u32 height = 0;
        u32 layers = 1;
        u32 mips   = 1;
        VkFormat format  = VK_FORMAT_UNDEFINED;
        bool     isDepth = false;

        // Layout the archive image is currently in; tracked so re-copies on subsequent
        // captures emit the right transition. The archive lives outside the RG state machine.
        VkImageLayout currentLayout = VK_IMAGE_LAYOUT_UNDEFINED;

        // Lazy ImGui descriptor set for displaying this archive in an ImGui::Image.
        // Allocated on first request via ImGui_ImplVulkan_AddTexture; freed in Destroy().
        VkDescriptorSet imguiDescSet = VK_NULL_HANDLE;

        // Lazy per-array-layer image views (e.g. cascade slices). Indexed by layer.
        // Empty unless the archive is sliced into the panel UI.
        std::vector<VkImageView> layerViews;

        // Phase 14F — lazy single-layer view creation for cascade slicing.
        // Returns VK_NULL_HANDLE if `layer` is out of range. Caches into
        // layerViews so repeated calls don't churn descriptors.
        VkImageView GetOrCreateLayerView(VkDevice device, u32 layer);

        void Destroy(VkDevice device, VmaAllocator allocator);
    };
}
