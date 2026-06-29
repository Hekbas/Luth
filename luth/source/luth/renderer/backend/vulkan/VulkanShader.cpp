#include "luthpch.h"
#include "VulkanShader.h"
#include "luth/renderer/shader/ShaderCompiler.h"
#include "luth/renderer/backend/vulkan/VulkanContext.h"
#include "luth/core/diagnostics/Log.h"

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

    static VkShaderStageFlagBits ToVkStage(ShaderStage stage)
    {
        switch (stage)
        {
            case ShaderStage::Vertex:   return VK_SHADER_STAGE_VERTEX_BIT;
            case ShaderStage::Fragment: return VK_SHADER_STAGE_FRAGMENT_BIT;
            case ShaderStage::Compute:  return VK_SHADER_STAGE_COMPUTE_BIT;
            default:                    return VkShaderStageFlagBits(0);
        }
    }

    VulkanShader::VulkanShader(ShaderStage stage, const std::vector<u32>& spirv, const fs::path& path)
        : m_Stage(stage), m_SpirV(spirv), m_Path(path)
    {
        if (m_Stage == ShaderStage::Unknown || m_SpirV.empty())
        {
            LH_LOG(Shaders, error, "VulkanShader: invalid stage or empty SPIR-V for '{}'", m_Path.string());
            return;
        }

        Reflect();
        CreateShaderModule();
    }

    VulkanShader::~VulkanShader()
    {
        Destroy();
    }

    void VulkanShader::Destroy()
    {
        if (m_ShaderModule != VK_NULL_HANDLE)
        {
            vkDestroyShaderModule(VulkanContext::Get().GetDevice(), m_ShaderModule, nullptr);
            m_ShaderModule = VK_NULL_HANDLE;
        }
        m_ShaderStageInfo = {};
    }

    void VulkanShader::Reload()
    {
        // VkShaderModule is consumed at pipeline-create time; previously-built
        // pipelines hold no reference to the handle (Vulkan spec). Destroying the
        // old module here without vkDeviceWaitIdle is safe -- in-flight pipelines
        // that link to this module's bytecode are unaffected, and the reload
        // callback in RenderPipeline defers their destruction via PushDeletion.
        Destroy();
        m_SpirV.clear();
        m_Buffers.clear();
        m_Resources.clear();
        m_PushConstants.clear();

        m_SpirV = ShaderCompiler::Compile(m_Path);
        if (m_SpirV.empty())
        {
            LH_LOG(Shaders, error, "VulkanShader: reload failed for '{}'", m_Path.string());
            return;
        }

        Reflect();
        CreateShaderModule();
        LH_LOG(Shaders, info, "VulkanShader: reloaded '{}'", m_Path.string());
    }

    VkShaderStageFlagBits VulkanShader::GetVkStage() const
    {
        return ToVkStage(m_Stage);
    }

    void VulkanShader::Reflect()
    {
#if LUTH_SPIRV_CROSS_ENABLED
        spirv_cross::Compiler compiler(m_SpirV.data(), m_SpirV.size());
        spirv_cross::ShaderResources resources = compiler.get_shader_resources();

        LH_LOG(Shaders, trace, "Reflecting Shader: {0} (Stage: {1})", m_Path.string(), (int)m_Stage);

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

            LH_LOG(Shaders, trace, "  Uniform Buffer: {0} (Set: {1}, Binding: {2}, Size: {3})", resource.name, set, binding, bufferSize);

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
                LH_LOG(Shaders, trace, "    Member: {0} (Offset: {1}, Size: {2})", memberName, memberOffset, memberSize);
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

            LH_LOG(Shaders, trace, "  Push Constant: {0} (Size: {1})", resource.name, bufferSize);

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
            LH_LOG(Shaders, trace, "  Texture: {0} (Set: {1}, Binding: {2})", resource.name, set, binding);
        }

        // One-line per-shader summary at Debug; the per-member detail above stays at Trace.
        LH_LOG(Shaders, debug, "reflected '{}' ({} UBOs, {} push, {} textures)",
            m_Path.filename().string(), resources.uniform_buffers.size(),
            resources.push_constant_buffers.size(), resources.sampled_images.size());
#else
        LH_LOG(Shaders, warn, "VulkanShader::Reflect() disabled -- spirv-cross not linked (ABI mismatch with Vulkan SDK pre-built libs)");
#endif
    }

    void VulkanShader::CreateShaderModule()
    {
        VkShaderModuleCreateInfo createInfo{};
        createInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
        createInfo.codeSize = m_SpirV.size() * sizeof(u32);
        createInfo.pCode = m_SpirV.data();

        if (vkCreateShaderModule(VulkanContext::Get().GetDevice(), &createInfo, nullptr, &m_ShaderModule) != VK_SUCCESS)
        {
            LH_LOG(Shaders, error, "Failed to create shader module for '{}'", m_Path.string());
            m_ShaderModule = VK_NULL_HANDLE;
            return;
        }

        m_ShaderStageInfo = {};
        m_ShaderStageInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        m_ShaderStageInfo.stage = ToVkStage(m_Stage);
        m_ShaderStageInfo.module = m_ShaderModule;
        m_ShaderStageInfo.pName = "main";
    }
}
