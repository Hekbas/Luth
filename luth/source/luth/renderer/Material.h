#pragma once

#include "luth/core/LuthTypes.h"
#include "luth/renderer/Shader.h"

#include <vector>
#include <filesystem>

namespace Luth
{
    enum class TextureType {
        Diffuse,
        Normal,
        Emissive,
        Metalness,
        Roughness,
        Specular
    };

    struct TextureInfo {
        TextureType type;
        fs::path path;
        u32 uvIndex = 0;
    };

    class Material
    {
    public:
        void SetShader(const std::shared_ptr<Shader>& shader) { m_Shader = shader; }
        const std::shared_ptr<Shader>& GetShader() const { return m_Shader; }

        void AddTexture(const TextureInfo& info) { m_Textures.push_back(info); }
        const std::vector<TextureInfo>& GetTextures() const { return m_Textures; }

        std::optional<u32> GetUVIndex(TextureType type) const {
            for (const auto& tex : m_Textures) {
                if (tex.type == type) return tex.uvIndex;
            }
            return std::nullopt; // No texture of this type
        }

    private:
        std::shared_ptr<Shader> m_Shader;
        std::vector<TextureInfo> m_Textures;
        std::string name;
    };
}
