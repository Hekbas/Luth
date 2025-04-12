#pragma once

#include "luth/core/LuthTypes.h"
#include "luth/core/UUID.h"
#include "luth/renderer/Renderer.h"
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
        Diffuse     = 0,
        Alpha       = 1,
        Normal      = 2,
        Metalness   = 3,
        Roughness   = 4,
        Specular    = 5,
        Oclusion    = 6,
        Emissive    = 7
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
        void SetTexture(const TextureInfo& texture) {
            int index = static_cast<int>(texture.type);
            if (index >= m_Textures.size()) AddTexture(texture);
            else m_Textures[index] = texture;
        }
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

        // Render mode
        RendererAPI::RenderMode GetRenderMode() const { return m_RenderMode; }
        void SetRenderMode(RendererAPI::RenderMode mode) { m_RenderMode = mode; }

        // Alpha cutoff for RenderMode::Cutout
        float GetAlphaCutoff() const { return m_AlphaCutoff; }
        void SetAlphaCutoff(float cutoff) { m_AlphaCutoff = cutoff; }

        // Blend factors
        void SetBlendSrc(RendererAPI::BlendFactor factor) { m_BlendSrc = factor; }
        RendererAPI::BlendFactor GetBlendSrc() const { return m_BlendSrc; }

        void SetBlendDst(RendererAPI::BlendFactor factor) { m_BlendDst = factor; }
        RendererAPI::BlendFactor GetBlendDst() const { return m_BlendDst; }

        // Serialization/Deserialization
        void Serialize(nlohmann::json& json) const;
        void Deserialize(const nlohmann::json& json);

        static const char* ToString(TextureType type);

    private:
        UUID m_ShaderUUID;
        std::vector<TextureInfo> m_Textures;

        RendererAPI::RenderMode m_RenderMode = RendererAPI::RenderMode::Opaque;
        float m_AlphaCutoff = 0.5f;
        RendererAPI::BlendFactor m_BlendSrc = RendererAPI::BlendFactor::SrcAlpha;
        RendererAPI::BlendFactor m_BlendDst = RendererAPI::BlendFactor::OneMinusSrcAlpha;
    };

    inline std::ostream& operator<<(std::ostream& os, const TextureType type) {
        return os << Material::ToString(type);
    }
}
