#pragma once

#include "luth/renderer/Shader.h"
#include <vulkan/vulkan.h>
#include <vector>

namespace Luth
{
    class VulkanShader : public Shader
    {
    public:
        VulkanShader(const fs::path& path);
        VulkanShader(const std::vector<u32>& vertSpv, const std::vector<u32>& fragSpv, const fs::path& path);
        virtual ~VulkanShader();

        virtual const fs::path& GetPath() const override { return m_Path; }
        void Reload() override;
        bool IsValid() const override;

        // Vulkan specific
        const std::vector<u32>& GetSpirV(VkShaderStageFlagBits stage) const;
        VkShaderModule GetShaderModule(VkShaderStageFlagBits stage) const;
        const std::vector<VkPipelineShaderStageCreateInfo>& GetShaderStages() const { return m_ShaderStages; }

    private:
        void CompileOrGetVulkanBinaries();
        void Reflect(VkShaderStageFlagBits stage, const std::vector<u32>& spirv);
        void CreateShaderModule(VkShaderStageFlagBits stage, const std::vector<u32>& spirv);

        fs::path m_Path;
        std::unordered_map<VkShaderStageFlagBits, std::vector<u32>> m_SpirVData;
        std::unordered_map<VkShaderStageFlagBits, VkShaderModule> m_ShaderModules;
        std::vector<VkPipelineShaderStageCreateInfo> m_ShaderStages;
    };
}
