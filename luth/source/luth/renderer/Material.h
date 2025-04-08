#pragma once

#include "luth/core/LuthTypes.h"
#include "luth/core/UUID.h"
#include "luth/resources/Resource.h"
#include "luth/resources/libraries/ShaderLibrary.h"
#include "luth/resources/libraries/TextureCache.h"

#include <nlohmann/json.hpp>
#include <vector>
#include <filesystem>
#include <iostream>

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
        UUID Uuid;
        TextureType type;
        u32 uvIndex = 0;
    };

    class Material : public Resource
    {
    public:
        // Shader management
        void SetShaderUUID(const UUID& uuid) { m_ShaderUUID = uuid; }
        UUID GetShaderUUID() const { return m_ShaderUUID; }
        std::shared_ptr<Shader> GetShader() const {
            return ShaderLibrary::Get(m_ShaderUUID);
        }

        // Texture management
        void AddTexture(const TextureInfo& texture) { m_Textures.push_back(texture); }
        void SetTexture(const TextureInfo& texture) { m_Textures[(int)texture.type] = texture; }
        const std::vector<TextureInfo>& GetTextures() const { return m_Textures; }

        std::optional<u32> GetUVIndex(TextureType type) const {
            for (const auto& tex : m_Textures) {
                if (tex.type == type) return tex.uvIndex;
            }
            return std::nullopt;
        }

        // Runtime texture access
        std::shared_ptr<Texture> GetTextureByType(TextureType type) const {
            for (const auto& tex : m_Textures) {
                if (tex.type == type) return TextureCache::Get(tex.Uuid);
            }
            return nullptr;
        }

        // Serialization/Deserialization
        void Serialize(nlohmann::json& json) const;
        void Deserialize(const nlohmann::json& json);

        static const char* ToString(TextureType type);

    private:
        UUID m_ShaderUUID;
        std::vector<TextureInfo> m_Textures;
    };

    inline std::ostream& operator<<(std::ostream& os, const TextureType type) {
        return os << Material::ToString(type);
    }
}
