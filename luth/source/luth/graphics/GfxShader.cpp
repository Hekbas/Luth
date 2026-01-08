#include "luthpch.h"
#include "luth/graphics/GfxShader.h"
#include "luth/core/Log.h"
#include <fstream>

namespace Luth::Gfx
{
    GfxShader::GfxShader(const std::string& path, ShaderStage stage)
        : m_Stage(stage)
    {
        auto code = ReadFile(path);
        CreateModule(code);
    }

    GfxShader::~GfxShader()
    {
        if (m_Module)
            vkDestroyShaderModule(GfxContext::Get().GetDevice(), m_Module, nullptr);
    }

    void GfxShader::CreateModule(const std::vector<char>& code)
    {
        VkShaderModuleCreateInfo createInfo{};
        createInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
        createInfo.codeSize = code.size();
        createInfo.pCode = reinterpret_cast<const uint32_t*>(code.data());

        if (vkCreateShaderModule(GfxContext::Get().GetDevice(), &createInfo, nullptr, &m_Module) != VK_SUCCESS)
        {
            LH_CORE_CRITICAL("Failed to create shader module!");
        }
    }

    std::vector<char> GfxShader::ReadFile(const std::string& filename)
    {
        std::ifstream file(filename, std::ios::ate | std::ios::binary);

        if (!file.is_open())
        {
            LH_CORE_CRITICAL("Failed to open shader file: {0}", filename);
            return {};
        }

        size_t fileSize = (size_t)file.tellg();
        std::vector<char> buffer(fileSize);

        file.seekg(0);
        file.read(buffer.data(), fileSize);
        file.close();

        return buffer;
    }
}
