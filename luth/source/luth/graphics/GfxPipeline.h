#pragma once

#include "luth/core/LuthTypes.h"
#include "luth/graphics/GfxContext.h"
#include "luth/graphics/GfxShader.h"
#include <vulkan/vulkan.h>
#include <vector>

namespace Luth::Gfx
{
    struct PipelineConfig
    {
        std::vector<VkFormat> colorFormats;
        VkFormat depthFormat = VK_FORMAT_UNDEFINED;
        
        bool depthTest = true;
        bool depthWrite = true;
        bool blend = false;
        
        VkPrimitiveTopology topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        VkPolygonMode polygonMode = VK_POLYGON_MODE_FILL;
        VkCullModeFlags cullMode = VK_CULL_MODE_BACK_BIT;
        VkFrontFace frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    };

    class GfxPipeline
    {
    public:
        GfxPipeline(const PipelineConfig& config, 
                    const std::shared_ptr<GfxShader>& vertShader, 
                    const std::shared_ptr<GfxShader>& fragShader,
                    VkDescriptorSetLayout descriptorLayout = VK_NULL_HANDLE); // TODO: Pipeline Layout Builder
        ~GfxPipeline();

        void Bind(VkCommandBuffer cmd);
        VkPipelineLayout GetLayout() const { return m_Layout; }

    private:
        void CreatePipeline(const PipelineConfig& config, 
                            const std::shared_ptr<GfxShader>& vertShader, 
                            const std::shared_ptr<GfxShader>& fragShader,
                            VkDescriptorSetLayout descriptorLayout);

        VkPipeline m_Pipeline = VK_NULL_HANDLE;
        VkPipelineLayout m_Layout = VK_NULL_HANDLE;
    };
}
