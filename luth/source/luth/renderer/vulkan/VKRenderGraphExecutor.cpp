#include "luthpch.h"
#include "luth/renderer/vulkan/VKRenderGraphExecutor.h"
#include "luth/core/Profiler.h"

namespace Luth
{
    VKRenderGraphExecutor::VKRenderGraphExecutor(VkDevice device, VkPhysicalDevice physicalDevice)
        : m_Device(device)
    {
        m_ResourceManager = std::make_unique<VKResourceManager>(device, physicalDevice);
    }

    VKRenderGraphExecutor::~VKRenderGraphExecutor()
    {
    }

    void VKRenderGraphExecutor::Execute(RG::RenderGraph& graph, VkCommandBuffer cmd)
    {
        LH_PROFILE_FUNCTION();

        // 1. Allocate physical resources for all virtual handles
        RealizeResources(graph);

        // 2. Execute Passes
        auto& passes = graph.GetPasses();
        auto& resources = graph.GetResources();

        for (const auto& pass : passes)
        {
            LH_PROFILE_SCOPE(pass.name.c_str());

            // Insert Barriers
            if (!pass.preBarriers.empty())
            {
                ExecuteBarriers(pass.preBarriers, cmd, graph);
            }

            // Prepare Context
            RG::RenderPassContext ctx;
            ctx.commandBuffer = cmd;
            
            // Provide resource lookup
            ctx.GetResource = [&](RG::ResourceHandle handle) -> void* {
                if (handle.index == 0 || handle.index > resources.size()) return nullptr;
                return resources[handle.index - 1].physicalResource;
            };

            // Execute Lambda
            pass.execute(ctx);
        }

        // 3. Cleanup (Release transient resources back to pool)
        for (auto& res : resources)
        {
            if (res.isTransient && res.physicalResource)
            {
                m_ResourceManager->ReleaseTexture((VKImageResource*)res.physicalResource);
                res.physicalResource = nullptr;
            }
        }
    }

    void VKRenderGraphExecutor::RealizeResources(RG::RenderGraph& graph)
    {
        auto& resources = graph.GetResources();
        for (auto& res : resources)
        {
            // If it's transient and not yet allocated
            if (res.isTransient && !res.physicalResource)
            {
                res.physicalResource = m_ResourceManager->GetTexture(res.desc);
            }
        }
    }

    void VKRenderGraphExecutor::ExecuteBarriers(const std::vector<RG::Barrier>& barriers, VkCommandBuffer cmd, RG::RenderGraph& graph)
    {
        std::vector<VkImageMemoryBarrier> imageBarriers;
        auto& resources = graph.GetResources();

        for (const auto& b : barriers)
        {
            auto& resNode = resources[b.resource.index - 1];
            VKImageResource* vkRes = (VKImageResource*)resNode.physicalResource;
            
            if (vkRes)
            {
                imageBarriers.push_back(CreateImageBarrier(
                    vkRes->image, 
                    b.before, 
                    b.after, 
                    resNode.desc.format
                ));
            }
        }

        if (!imageBarriers.empty())
        {
            vkCmdPipelineBarrier(
                cmd,
                VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, // TODO: Optimize stages
                VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                0,
                0, nullptr,
                0, nullptr,
                (uint32_t)imageBarriers.size(), imageBarriers.data()
            );
        }
    }

    VkImageMemoryBarrier VKRenderGraphExecutor::CreateImageBarrier(
        VkImage image, 
        RG::ResourceState oldState, 
        RG::ResourceState newState,
        RG::TextureFormat format)
    {
        VkImageMemoryBarrier barrier{};
        barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED; // TODO: Map state to layout
        barrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;   // TODO: Map state to layout
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.image = image;
        
        if (format == RG::TextureFormat::D32_Float || format == RG::TextureFormat::D24_Unorm_S8_Uint)
            barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
        else
            barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;

        barrier.subresourceRange.baseMipLevel = 0;
        barrier.subresourceRange.levelCount = 1;
        barrier.subresourceRange.baseArrayLayer = 0;
        barrier.subresourceRange.layerCount = 1;

        // State to Layout/Access Mapping
        auto GetLayout = [](RG::ResourceState state) -> VkImageLayout {
            switch (state) {
                case RG::ResourceState::Undefined: return VK_IMAGE_LAYOUT_UNDEFINED;
                case RG::ResourceState::ColorAttachment: return VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
                case RG::ResourceState::DepthStencilAttachment: return VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
                case RG::ResourceState::ShaderResource: return VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
                case RG::ResourceState::TransferDst: return VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
                case RG::ResourceState::Present: return VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
                default: return VK_IMAGE_LAYOUT_GENERAL;
            }
        };

        auto GetAccess = [](RG::ResourceState state) -> VkAccessFlags {
            switch (state) {
                case RG::ResourceState::Undefined: return 0;
                case RG::ResourceState::ColorAttachment: return VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
                case RG::ResourceState::DepthStencilAttachment: return VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
                case RG::ResourceState::ShaderResource: return VK_ACCESS_SHADER_READ_BIT;
                case RG::ResourceState::TransferDst: return VK_ACCESS_TRANSFER_WRITE_BIT;
                default: return 0;
            }
        };

        barrier.oldLayout = GetLayout(oldState);
        barrier.newLayout = GetLayout(newState);
        barrier.srcAccessMask = GetAccess(oldState);
        barrier.dstAccessMask = GetAccess(newState);

        return barrier;
    }
}
