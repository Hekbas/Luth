#include "luthpch.h"
#include "VulkanShader.h"
#include "luth/renderer/ShaderCompiler.h"
#include "luth/renderer/backend/vulkan/VulkanContext.h"
#include "luth/core/Log.h"

#if LUTH_SPIRV_CROSS_ENABLED
#include <spirv_cross.hpp>
#include <spirv_glsl.hpp>
#endif

namespace Luth
{
#if LUTH_SPIRV_CROSS_ENABLED
    static ShaderDataType SpirvTypeToShaderDataType(const spirv_cross::SPIRType& type)
    {
        switch (type.basetype)
        {
            case spirv_cross::SPIRType::Boolean: return ShaderDataType::Bool;
            case spirv_cross::SPIRType::Int:
                if (type.vecsize == 1) return ShaderDataType::Int;
                if (type.vecsize == 2) return ShaderDataType::Int2;
                if (type.vecsize == 3) return ShaderDataType::Int3;
                if (type.vecsize == 4) return ShaderDataType::Int4;
                break;
            case spirv_cross::SPIRType::Float:
                if (type.columns == 3) return ShaderDataType::Mat3;
                if (type.columns == 4) return ShaderDataType::Mat4;
                if (type.vecsize == 1) return ShaderDataType::Float;
                if (type.vecsize == 2) return ShaderDataType::Float2;
                if (type.vecsize == 3) return ShaderDataType::Float3;
                if (type.vecsize == 4) return ShaderDataType::Float4;
                break;
        }
        return ShaderDataType::None;
    }
#endif

    VulkanShader::VulkanShader(const fs::path& path)
        : m_Path(path)
    {
        CompileOrGetVulkanBinaries();
    }

    VulkanShader::VulkanShader(const std::vector<u32>& vertSpv, const std::vector<u32>& fragSpv, const fs::path& path)
        : m_Path(path)
    {
        if (!vertSpv.empty())
        {
            m_SpirVData[VK_SHADER_STAGE_VERTEX_BIT] = vertSpv;
            Reflect(VK_SHADER_STAGE_VERTEX_BIT, vertSpv);
            CreateShaderModule(VK_SHADER_STAGE_VERTEX_BIT, vertSpv);
        }

        if (!fragSpv.empty())
        {
            m_SpirVData[VK_SHADER_STAGE_FRAGMENT_BIT] = fragSpv;
            Reflect(VK_SHADER_STAGE_FRAGMENT_BIT, fragSpv);
            CreateShaderModule(VK_SHADER_STAGE_FRAGMENT_BIT, fragSpv);
        }
    }

    VulkanShader::~VulkanShader()
    {
        VkDevice device = VulkanContext::Get().GetDevice();
        for (auto& [stage, module] : m_ShaderModules)
        {
            vkDestroyShaderModule(device, module, nullptr);
        }
    }

    void VulkanShader::Reload()
    {
        VkDevice device = VulkanContext::Get().GetDevice();

        // Destroy old shader modules
        for (auto& [stage, module] : m_ShaderModules)
            vkDestroyShaderModule(device, module, nullptr);

        m_ShaderModules.clear();
        m_ShaderStages.clear();
        m_SpirVData.clear();
        m_Buffers.clear();
        m_Resources.clear();
        m_PushConstants.clear();

        // Recompile from disk
        CompileOrGetVulkanBinaries();

        if (IsValid())
            LH_CORE_INFO("VulkanShader: reloaded '{}'", m_Path.string());
        else
            LH_CORE_ERROR("VulkanShader: reload failed for '{}'", m_Path.string());
    }

    bool VulkanShader::IsValid() const
    {
        return !m_ShaderModules.empty();
    }

    const std::vector<u32>& VulkanShader::GetSpirV(VkShaderStageFlagBits stage) const
    {
        static const std::vector<u32> empty;
        auto it = m_SpirVData.find(stage);
        return (it != m_SpirVData.end()) ? it->second : empty;
    }

    VkShaderModule VulkanShader::GetShaderModule(VkShaderStageFlagBits stage) const
    {
        auto it = m_ShaderModules.find(stage);
        return (it != m_ShaderModules.end()) ? it->second : VK_NULL_HANDLE;
    }

    void VulkanShader::CompileOrGetVulkanBinaries()
    {
        // For now, we assume the path points to a file that might have .vert/.frag extensions
        // In a real asset system, we might bundle them. 
        // Here we try to deduce the stage from the file or load related files.
        // Simplification: Assume m_Path is the base name or we check for extensions.
        
        // Check for Vertex Shader
        fs::path vertPath = m_Path;
        if (vertPath.extension() != ".vert") vertPath.replace_extension(".vert");
        
        if (fs::exists(vertPath))
        {
            std::vector<u32> spirv = ShaderCompiler::Compile(vertPath);
            if (!spirv.empty())
            {
                m_SpirVData[VK_SHADER_STAGE_VERTEX_BIT] = spirv;
                Reflect(VK_SHADER_STAGE_VERTEX_BIT, spirv);
                CreateShaderModule(VK_SHADER_STAGE_VERTEX_BIT, spirv);
            }
        }

        // Check for Fragment Shader
        fs::path fragPath = m_Path;
        if (fragPath.extension() != ".frag") fragPath.replace_extension(".frag");

        if (fs::exists(fragPath))
        {
            std::vector<u32> spirv = ShaderCompiler::Compile(fragPath);
            if (!spirv.empty())
            {
                m_SpirVData[VK_SHADER_STAGE_FRAGMENT_BIT] = spirv;
                Reflect(VK_SHADER_STAGE_FRAGMENT_BIT, spirv);
                CreateShaderModule(VK_SHADER_STAGE_FRAGMENT_BIT, spirv);
            }
        }
    }

    void VulkanShader::Reflect(VkShaderStageFlagBits stage, const std::vector<u32>& spirv)
    {
#if LUTH_SPIRV_CROSS_ENABLED
        spirv_cross::Compiler compiler(spirv.data(), spirv.size());
        spirv_cross::ShaderResources resources = compiler.get_shader_resources();

        LH_CORE_TRACE("Reflecting Shader: {0} (Stage: {1})", m_Path.string(), (int)stage);

        // Uniform Buffers
        for (const auto& resource : resources.uniform_buffers)
        {
            const auto& bufferType = compiler.get_type(resource.base_type_id);
            uint32_t bufferSize = (uint32_t)compiler.get_declared_struct_size(bufferType);
            uint32_t set = compiler.get_decoration(resource.id, spv::DecorationDescriptorSet);
            uint32_t binding = compiler.get_decoration(resource.id, spv::DecorationBinding);
            int memberCount = (int)bufferType.member_types.size();

            ShaderBuffer buffer;
            buffer.Name = resource.name;
            buffer.Set = set;
            buffer.Binding = binding;
            buffer.Size = bufferSize;

            LH_CORE_TRACE("  Uniform Buffer: {0} (Set: {1}, Binding: {2}, Size: {3})", resource.name, set, binding, bufferSize);

            for (int i = 0; i < memberCount; i++)
            {
                const auto& memberType = compiler.get_type(bufferType.member_types[i]);
                const std::string& memberName = compiler.get_member_name(bufferType.self, i);
                uint32_t memberSize = (uint32_t)compiler.get_declared_struct_member_size(bufferType, i);
                uint32_t memberOffset = (uint32_t)compiler.type_struct_member_offset(bufferType, i);

                ShaderUniform uniform;
                uniform.Name = memberName;
                uniform.Type = SpirvTypeToShaderDataType(memberType);
                uniform.Size = memberSize;
                uniform.Offset = memberOffset;

                buffer.Uniforms[memberName] = uniform;
                LH_CORE_TRACE("    Member: {0} (Offset: {1}, Size: {2})", memberName, memberOffset, memberSize);
            }

            m_Buffers[resource.name] = buffer;
        }

        // Push Constants
        for (const auto& resource : resources.push_constant_buffers)
        {
            const auto& bufferType = compiler.get_type(resource.base_type_id);
            uint32_t bufferSize = (uint32_t)compiler.get_declared_struct_size(bufferType);
            int memberCount = (int)bufferType.member_types.size();

            ShaderBuffer buffer;
            buffer.Name = resource.name;
            buffer.Size = bufferSize;

            LH_CORE_TRACE("  Push Constant: {0} (Size: {1})", resource.name, bufferSize);

            for (int i = 0; i < memberCount; i++)
            {
                const auto& memberType = compiler.get_type(bufferType.member_types[i]);
                const std::string& memberName = compiler.get_member_name(bufferType.self, i);
                uint32_t memberSize = (uint32_t)compiler.get_declared_struct_member_size(bufferType, i);
                uint32_t memberOffset = (uint32_t)compiler.type_struct_member_offset(bufferType, i);

                ShaderUniform uniform;
                uniform.Name = memberName;
                uniform.Type = SpirvTypeToShaderDataType(memberType);
                uniform.Size = memberSize;
                uniform.Offset = memberOffset;

                buffer.Uniforms[memberName] = uniform;
            }

            m_PushConstants[resource.name] = buffer;
        }

        // Sampled Images (Textures)
        for (const auto& resource : resources.sampled_images)
        {
            uint32_t set = compiler.get_decoration(resource.id, spv::DecorationDescriptorSet);
            uint32_t binding = compiler.get_decoration(resource.id, spv::DecorationBinding);

            const auto& type = compiler.get_type(resource.type_id);
            uint32_t arraySize = !type.array.empty() ? type.array[0] : 1;
            if (arraySize == 0) arraySize = 0;

            ShaderResource res;
            res.Name = resource.name;
            res.Set = set;
            res.Binding = binding;
            res.ArraySize = arraySize;

            m_Resources[resource.name] = res;
            LH_CORE_TRACE("  Texture: {0} (Set: {1}, Binding: {2})", resource.name, set, binding);
        }
#else
        (void)stage; (void)spirv;
        LH_CORE_WARN("VulkanShader::Reflect() disabled — spirv-cross not linked (ABI mismatch with Vulkan SDK pre-built libs)");
#endif
    }

    void VulkanShader::CreateShaderModule(VkShaderStageFlagBits stage, const std::vector<u32>& spirv)
    {
        VkShaderModuleCreateInfo createInfo{};
        createInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
        createInfo.codeSize = spirv.size() * sizeof(u32);
        createInfo.pCode = spirv.data();

        VkShaderModule module;
        if (vkCreateShaderModule(VulkanContext::Get().GetDevice(), &createInfo, nullptr, &module) != VK_SUCCESS)
        {
            LH_CORE_ERROR("Failed to create shader module!");
            return;
        }

        m_ShaderModules[stage] = module;

        VkPipelineShaderStageCreateInfo stageInfo{};
        stageInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stageInfo.stage = stage;
        stageInfo.module = module;
        stageInfo.pName = "main";
        m_ShaderStages.push_back(stageInfo);
    }
}
