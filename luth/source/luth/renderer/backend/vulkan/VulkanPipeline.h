#pragma once

#include "luth/core/LuthTypes.h"
#include <vulkan/vulkan.h>
#include <vector>
#include <string>

namespace Luth
{
    struct PipelineConfig
    {
        std::vector<VkFormat> colorFormats;
        VkFormat depthFormat = VK_FORMAT_UNDEFINED;
        
        bool depthTest = true;
        bool depthWrite = true;
        VkCompareOp depthCompareOp = VK_COMPARE_OP_LESS;

        bool blendEnabled = false;
        VkCullModeFlags cullMode = VK_CULL_MODE_BACK_BIT;
        VkFrontFace frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
        
        VkPrimitiveTopology topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    };

    class VKPipeline
    {
    public:
        VKPipeline(const PipelineConfig& config, 
                   const std::string& vertPath, 
                   const std::string& fragPath,
                   VkDescriptorSetLayout globalLayout); // Pass bindless layout
        ~VKPipeline();

        void Bind(VkCommandBuffer cmd);
        VkPipelineLayout GetLayout() const { return m_PipelineLayout; }

    private:
        void CreatePipeline(const PipelineConfig& config, 
                            const std::string& vertPath, 
                            const std::string& fragPath,
                            VkDescriptorSetLayout globalLayout);
        VkShaderModule LoadShader(const std::string& path);

        VkPipeline m_Pipeline = VK_NULL_HANDLE;
        VkPipelineLayout m_PipelineLayout = VK_NULL_HANDLE;
        VkDevice m_Device = VK_NULL_HANDLE;
    };
}