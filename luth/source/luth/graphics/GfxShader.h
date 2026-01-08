#pragma once

#include "luth/core/LuthTypes.h"
#include "luth/graphics/GfxContext.h"
#include <vulkan/vulkan.h>
#include <string>
#include <vector>

namespace Luth::Gfx
{
    enum class ShaderStage
    {
        Vertex,
        Fragment,
        Compute
    };

    class GfxShader
    {
    public:
        GfxShader(const std::string& path, ShaderStage stage);
        ~GfxShader();

        VkShaderModule GetModule() const { return m_Module; }
        ShaderStage GetStage() const { return m_Stage; }
        const std::string& GetEntryPoints() const { return m_EntryPoint; }

    private:
        void CreateModule(const std::vector<char>& code);
        std::vector<char> ReadFile(const std::string& filename);

        VkShaderModule m_Module = VK_NULL_HANDLE;
        ShaderStage m_Stage;
        std::string m_EntryPoint = "main";
    };
}
