#pragma once

#include "luth/core/types/LuthTypes.h"
#include <vulkan/vulkan.h>
#include <vector>
#include <span>

namespace Luth
{
    struct AttachmentInfo
    {
        VkImageView ImageView = VK_NULL_HANDLE;
        VkFormat Format = VK_FORMAT_UNDEFINED; // Added Format
        VkAttachmentLoadOp LoadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        VkAttachmentStoreOp StoreOp = VK_ATTACHMENT_STORE_OP_STORE;
        VkClearValue ClearValue = {};
        VkImageLayout Layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        VkResolveModeFlagBits ResolveMode = VK_RESOLVE_MODE_NONE;
        VkImageView ResolveImageView = VK_NULL_HANDLE;
    };

    struct RenderPassInfo
    {
        std::span<AttachmentInfo> ColorAttachments;
        AttachmentInfo* DepthAttachment = nullptr;
        AttachmentInfo* StencilAttachment = nullptr;
        
        VkRect2D RenderArea = {};
        u32 LayerCount = 1;
        VkRenderingFlags Flags = 0; // Added Flags
    };

    class DynamicRendering
    {
    public:
        static void BeginRendering(VkCommandBuffer cmd, const RenderPassInfo& info)
        {
            std::vector<VkRenderingAttachmentInfo> colorInfos;
            colorInfos.reserve(info.ColorAttachments.size());

            for (const auto& att : info.ColorAttachments)
            {
                VkRenderingAttachmentInfo colorInfo{};
                colorInfo.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
                colorInfo.imageView = att.ImageView;
                colorInfo.imageLayout = att.Layout;
                colorInfo.loadOp = att.LoadOp;
                colorInfo.storeOp = att.StoreOp;
                colorInfo.clearValue = att.ClearValue;
                colorInfo.resolveMode = att.ResolveMode;
                colorInfo.resolveImageView = att.ResolveImageView;
                colorInfo.resolveImageLayout = (att.ResolveImageView != VK_NULL_HANDLE) ? VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED;
                
                colorInfos.push_back(colorInfo);
            }

            VkRenderingAttachmentInfo depthInfo{};
            if (info.DepthAttachment)
            {
                depthInfo.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
                depthInfo.imageView = info.DepthAttachment->ImageView;
                depthInfo.imageLayout = info.DepthAttachment->Layout;
                depthInfo.loadOp = info.DepthAttachment->LoadOp;
                depthInfo.storeOp = info.DepthAttachment->StoreOp;
                depthInfo.clearValue = info.DepthAttachment->ClearValue;
            }

            VkRenderingAttachmentInfo stencilInfo{};
            if (info.StencilAttachment)
            {
                stencilInfo.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
                stencilInfo.imageView = info.StencilAttachment->ImageView;
                stencilInfo.imageLayout = info.StencilAttachment->Layout;
                stencilInfo.loadOp = info.StencilAttachment->LoadOp;
                stencilInfo.storeOp = info.StencilAttachment->StoreOp;
                stencilInfo.clearValue = info.StencilAttachment->ClearValue;
            }

            VkRenderingInfo renderingInfo{};
            renderingInfo.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
            renderingInfo.flags = info.Flags; // Use flags
            renderingInfo.renderArea = info.RenderArea;
            renderingInfo.layerCount = info.LayerCount;
            renderingInfo.colorAttachmentCount = (u32)colorInfos.size();
            renderingInfo.pColorAttachments = colorInfos.data();
            renderingInfo.pDepthAttachment = info.DepthAttachment ? &depthInfo : nullptr;
            renderingInfo.pStencilAttachment = info.StencilAttachment ? &stencilInfo : nullptr;

            vkCmdBeginRendering(cmd, &renderingInfo);
        }

        static void EndRendering(VkCommandBuffer cmd)
        {
            vkCmdEndRendering(cmd);
        }
    };
}
