#pragma once

#include "luth/renderer/shader/Shader.h"
#include <vulkan/vulkan.h>
#include <vector>

namespace Luth
{
    // Concrete Shader implementation backed by a VkShaderModule. Owns the SPIR-V blob and the
    // pre-built VkPipelineShaderStageCreateInfo so VKPipeline construction can fold this in
    // directly. Reload tears down the module and rebuilds from the new SPIR-V — used by
    // ShaderWatcher to support hot reload without restarting the engine.
    class VulkanShader : public Shader
    {
    public:
        VulkanShader(ShaderStage stage, const std::vector<u32>& spirv, const fs::path& path);
        virtual ~VulkanShader();

        // Shader interface
        ShaderStage GetStage() const override { return m_Stage; }
        const std::vector<u32>& GetSpirV() const override { return m_SpirV; }
        const fs::path& GetPath() const override { return m_Path; }
        bool IsValid() const override { return m_ShaderModule != VK_NULL_HANDLE; }
        void Reload() override;

        // Vulkan specific
        VkShaderModule GetShaderModule() const { return m_ShaderModule; }
        const VkPipelineShaderStageCreateInfo& GetShaderStageInfo() const { return m_ShaderStageInfo; }
        VkShaderStageFlagBits GetVkStage() const;

    private:
        void Reflect();
        void CreateShaderModule();
        void Destroy();

        ShaderStage      m_Stage = ShaderStage::Unknown;
        std::vector<u32> m_SpirV;
        fs::path         m_Path;
        VkShaderModule   m_ShaderModule = VK_NULL_HANDLE;
        VkPipelineShaderStageCreateInfo m_ShaderStageInfo{};
    };
}
