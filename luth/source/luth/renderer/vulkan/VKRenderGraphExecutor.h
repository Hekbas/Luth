#pragma once

#include "luth/renderer/rendergraph/RenderGraph.h"
#include "luth/renderer/vulkan/VKResourceManager.h"
#include "luth/renderer/vulkan/VKDevice.h"

#include <vulkan/vulkan.h>

namespace Luth
{
    class VKRenderGraphExecutor
    {
    public:
        VKRenderGraphExecutor(VkDevice device, VkPhysicalDevice physicalDevice);
        ~VKRenderGraphExecutor();

        void Execute(RG::RenderGraph& graph, VkCommandBuffer cmd);

    private:
        void RealizeResources(RG::RenderGraph& graph);
        void ExecuteBarriers(const std::vector<RG::Barrier>& barriers, VkCommandBuffer cmd, RG::RenderGraph& graph);
        
        VkImageMemoryBarrier CreateImageBarrier(
            VkImage image, 
            RG::ResourceState oldState, 
            RG::ResourceState newState,
            RG::TextureFormat format);

        std::unique_ptr<VKResourceManager> m_ResourceManager;
        VkDevice m_Device;
    };
}
