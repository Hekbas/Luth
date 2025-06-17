#pragma once

#include "luth/core/LuthTypes.h"
#include "luth/core/UUID.h"
#include "luth/renderer/RendererAPI.h"
#include "luth/resources/Resource.h"
#include "luth/resources/libraries/ShaderLibrary.h"
#include "luth/resources/libraries/TextureCache.h"

#include <nlohmann/json.hpp>
#include <vector>
#include <filesystem>
#include <iostream>

namespace Luth
{
    enum class MapType {
        Diffuse     = 0,
        Alpha       = 1,
        Normal      = 2,
        Metalness   = 3,
        Roughness   = 4,
        Specular    = 5,
        Oclusion    = 6,
        Emissive    = 7,
        Thickness   = 8
    };

    struct MapInfo {
        UUID Uuid;
        MapType type;
        u32 uvIndex = 0;
        bool useMap = true;
        bool useTexture;
    };

    struct Subsurface {
        Vec3 color = Vec3(0.0f);
        float strength = 1.0f;
        float thicknessScale = 1.0f;
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

        // Map management
        void AddTexture(const MapInfo& texture) { m_Maps.push_back(texture); }
        void SetTexture(const MapInfo& texture) {
            int index = static_cast<int>(texture.type);
            if (index >= m_Maps.size()) AddTexture(texture);
            else m_Maps[index] = texture;
        }
        const std::vector<MapInfo>& GetTextures() const { return m_Maps; }

        std::optional<u32> GetUVIndex(MapType type) const {
            for (const auto& tex : m_Maps) {
                if (tex.type == type) return tex.uvIndex;
            }
            return std::nullopt;
        }

        void EnableUseMap(MapType type, bool enable) {
            for (auto& tex : m_Maps) {
                if (tex.type == type) tex.useMap = enable;
            }
        }
        bool IsUseMapEnabled(MapType type) const {
            for (const auto& tex : m_Maps) {
                if (tex.type == type) return tex.useMap;
            }
        }

        void EnableUseTexture(MapType type, bool enable) {
            for (auto& tex : m_Maps) {
                if (tex.type == type) tex.useTexture = enable;
            }
        }
        bool IsUseTextureEnabled(MapType type) const {
            for (const auto& tex : m_Maps) {
                if (tex.type == type) return tex.useTexture;
            }
        }

        // Runtime texture access
        std::shared_ptr<Texture> GetTextureByType(MapType type) const {
            for (const auto& tex : m_Maps) {
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

        void EnableAlphaFromDiffuse(bool enable) { m_AlphaFromDiffuse = enable; }
        bool IsAlphaFromDiffuseEnabled() const { return m_AlphaFromDiffuse; }

        // Properties
        Vec4 GetColor() const { return m_Color; }
        void SetColor(Vec4 color) { m_Color = color; }

        float GetAlpha() const { return m_Alpha; }
        void SetAlpha(float alpha) { m_Alpha = alpha; }

        float GetMetal() const { return m_Metal; }
        void SetMetal(float metal) { m_Metal = metal; }

        float GetRough() const { return m_Rough; }
        void SetRough(float rough) { m_Rough = rough; }

        Vec3 GetEmissive() const { return m_Emissive; }
        void SetEmissive(Vec3 emissive) { m_Emissive = emissive; }

        Subsurface GetSubsurface() const { return m_Subsurface; }
        void SetSubsurfaceParams(const Vec3& color, float strength, float thickness) {
            m_Subsurface.color = color;
            m_Subsurface.strength = strength;
            m_Subsurface.thicknessScale = thickness;
        }
        void SetSubsurfaceColor(const Vec3& color) { m_Subsurface.color = color; }
        void SetSubsurfaceStrength(float strength) { m_Subsurface.strength = strength; }
        void SetSubsurfaceThicknessScale(float thickness) { m_Subsurface.thicknessScale = thickness; }

        bool IsGloss() const { return m_IsGloss; }
        void SetGloss(bool gloss) { m_IsGloss = gloss; }

        bool IsSingleChannel() const { return m_IsSingleChannel; }
        void SetSingleChannel(bool singleChannel) { m_IsSingleChannel = singleChannel; }

        // Serialization/Deserialization
        void Serialize(nlohmann::json& json) const;
        void Deserialize(const nlohmann::json& json);

        static const char* ToString(MapType type);

    private:
        UUID m_ShaderUUID;
        std::vector<MapInfo> m_Maps;

        RendererAPI::RenderMode m_RenderMode = RendererAPI::RenderMode::Opaque;
        RendererAPI::BlendFactor m_BlendSrc = RendererAPI::BlendFactor::SrcAlpha;
        RendererAPI::BlendFactor m_BlendDst = RendererAPI::BlendFactor::OneMinusSrcAlpha;
        float m_AlphaCutoff = 0.5f;
        bool m_AlphaFromDiffuse = false;
        bool m_IsGloss = false;
        bool m_IsSingleChannel = false;

        Vec4 m_Color = Vec4(1.0f);
        float m_Alpha = 1.0f;
        float m_Metal = 0.5f;
        float m_Rough = 0.5f;
        Vec3 m_Emissive = Vec3(0.0f);
        Subsurface m_Subsurface;
    };

    inline std::ostream& operator<<(std::ostream& os, const MapType type) {
        return os << Material::ToString(type);
    }
}
