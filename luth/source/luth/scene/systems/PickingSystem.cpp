#include "luthpch.h"
#include "luth/scene/systems/PickingSystem.h"
#include "luth/scene/systems/RenderingSystem.h"
#include "luth/scene/systems/SystemRegistry.h"
#include "luth/renderer/RenderPipeline.h"
#include "luth/renderer/FrameTargets.h"
#include "luth/renderer/backend/vulkan/VulkanContext.h"
#include "luth/renderer/backend/vulkan/VulkanTexture.h"
#include "luth/renderer/backend/vulkan/VulkanAllocator.h"

#include <vma/vk_mem_alloc.h>

namespace Luth
{
    void PickingSystem::RequestPick(int x, int y)
    {
        m_Coord   = { x, y };
        m_Pending = true;
        m_Ready   = false;
    }

    entt::entity PickingSystem::ConsumeResult()
    {
        m_Ready = false;
        return m_Picked;
    }

    void PickingSystem::Update(Scene*)
    {
        if (!m_Pending) return;
        m_Pending = false;

        auto* rs = SystemRegistry::GetSystem<RenderingSystem>();
        if (!rs) return;

        auto& targets  = rs->GetFrameTargets();
        auto& pipeline = rs->GetPipeline();

        const auto& entityIDTex = targets.GetEntityIDBuffer();
        if (!entityIDTex) return;

        const int px = m_Coord.x;
        const int py = m_Coord.y;
        if (px < 0 || py < 0 || px >= (int)entityIDTex->GetWidth() || py >= (int)entityIDTex->GetHeight())
            return;

        auto vkID = std::static_pointer_cast<VKTexture>(entityIDTex);

        VkBuffer stagingBuf;
        VkBufferCreateInfo bufInfo{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
        bufInfo.size  = sizeof(u32);
        bufInfo.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        VmaAllocation stagingAlloc = VulkanAllocator::AllocateBuffer(bufInfo, VMA_MEMORY_USAGE_GPU_TO_CPU, stagingBuf);

        VulkanContext::Get().ImmediateSubmit([&](VkCommandBuffer cmd)
        {
            VkImageMemoryBarrier2 barrier{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2 };
            barrier.srcStageMask  = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
            barrier.srcAccessMask = VK_ACCESS_2_SHADER_READ_BIT;
            barrier.dstStageMask  = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
            barrier.dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT;
            barrier.oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
            barrier.image = vkID->GetImage();
            barrier.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };

            VkDependencyInfo dep{ VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
            dep.imageMemoryBarrierCount = 1;
            dep.pImageMemoryBarriers = &barrier;
            vkCmdPipelineBarrier2(cmd, &dep);

            VkBufferImageCopy region{};
            region.bufferOffset = 0;
            region.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
            region.imageOffset = { px, py, 0 };
            region.imageExtent = { 1, 1, 1 };
            vkCmdCopyImageToBuffer(cmd, vkID->GetImage(), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, stagingBuf, 1, &region);
        });

        void* mapped = VulkanAllocator::Map(stagingAlloc);
        u32 entityIdx = *reinterpret_cast<u32*>(mapped);
        VulkanAllocator::Unmap(stagingAlloc);
        VulkanAllocator::FreeBuffer(stagingBuf, stagingAlloc);

        const auto& entityLookup = pipeline.GetEntityLookup();
        m_Picked = (entityIdx > 0 && entityIdx < (u32)entityLookup.size())
                   ? entityLookup[entityIdx]
                   : entt::null;
        m_Ready = true;
    }
}
