#pragma once

#include "luth/core/LuthTypes.h"
#include "luth/core/UUID.h"
#include "luth/resources/Asset.h"
#include "luth/resources/AssetManager.h"
#include "luth/renderer/shader/Shader.h"
#include "luth/renderer/resources/Texture.h"

#include <nlohmann/json.hpp>
#include <vector>
#include <filesystem>
#include <iostream>
#include <optional>

namespace Luth
{
    enum class MapType {
        Diffuse     = 0,
        Alpha       = 1,
        Normal      = 2,
        Metalness   = 3,
        Roughness   = 4,
        Specular    = 5,
        Occlusion   = 6,
        Emissive    = 7,
        Thickness   = 8
    };

    struct MapInfo {
        UUID Uuid;
        MapType type;
        u32 uvIndex = 0;
        bool useMap = true;
        bool useTexture = true;
        
        // Bindless Index (Runtime only)
        u32 bindlessIndex = 0; 
    };

    // GPU-friendly Material Data Structure (Std140/Std430)
    // This matches the shader struct
    struct GPUMaterialData
    {
        Vec4 color = { 1.0f, 1.0f, 1.0f, 1.0f };
        
        // Texture Indices (Bindless)
        u32 diffuseIndex = 0;
        u32 normalIndex = 0;
        u32 metalRoughIndex = 0;
        u32 occlusionIndex = 0;
        
        // Factors
        f32 metalness = 0.0f;
        f32 roughness = 0.5f;
        f32 alphaCutoff = 0.5f;
        u32 flags = 0; // Bitmask for features
    };

    class Material : public Asset
    {
    public:
        virtual AssetType GetType() const override { return AssetType::Material; }
        
        enum class RenderMode { Opaque, Cutout, Transparent, Fade };
        enum class BlendFactor { Zero, One, SrcAlpha, OneMinusSrcAlpha, DstAlpha, OneMinusDstAlpha };
        enum class CullMode { Back, Front, None };

        // Shader management
        void SetShader(const UUID& uuid);
        UUID GetShaderUUID() const { return m_ShaderUUID; }
        std::shared_ptr<Shader> GetShader() const {
             return AssetManager::GetAsset<Shader>(m_ShaderUUID);
        }

        // Map management
        void AddTexture(const MapInfo& texture) { m_Maps.push_back(texture); }
        
        void SetTexture(const MapInfo& texture) {
            bool found = false;
            for (auto& map : m_Maps) {
                if (map.type == texture.type) {
                    map = texture;
                    found = true;
                    break;
                }
            }
            if (!found) m_Maps.push_back(texture);
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
            return false;
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
            return false;
        }

        // Runtime texture access
        std::shared_ptr<Texture> GetTextureByType(MapType type) const {
            for (const auto& tex : m_Maps) {
                if (tex.type == type) return AssetManager::GetAsset<Texture>(tex.Uuid);
            }
            return nullptr;
        }

        // Render mode
        RenderMode GetRenderMode() const { return m_RenderMode; }
        void SetRenderMode(RenderMode mode) { m_RenderMode = mode; }

        // Alpha cutoff for RenderMode::Cutout
        float GetAlphaCutoff() const { return m_AlphaCutoff; }
        void SetAlphaCutoff(float cutoff) { m_AlphaCutoff = cutoff; }

        // Blend factors
        void SetBlendSrc(BlendFactor factor) { m_BlendSrc = factor; }
        BlendFactor GetBlendSrc() const { return m_BlendSrc; }

        void SetBlendDst(BlendFactor factor) { m_BlendDst = factor; }
        BlendFactor GetBlendDst() const { return m_BlendDst; }

        // Face culling
        CullMode GetCullMode() const { return m_CullMode; }
        void SetCullMode(CullMode mode) { m_CullMode = mode; }

        void EnableAlphaFromDiffuse(bool enable) { m_AlphaFromDiffuse = enable; }
        bool IsAlphaFromDiffuseEnabled() const { return m_AlphaFromDiffuse; }

        // Generic Uniform Access
        template<typename T>
        void Set(const std::string& name, const T& value) {
            SetUniformData(name, &value, sizeof(T));
        }

        template<typename T>
        T Get(const std::string& name, T defaultValue = T()) const {
            T value;
            if (GetUniformData(name, &value, sizeof(T)))
                return value;
            return defaultValue;
        }

        const std::vector<uint8_t>& GetUniformStorage() const { return m_UniformStorage; }
        
        // Albedo color (direct access — bypasses uniform storage)
        Vec4 GetColor() const { return m_GPUData.color; }
        void SetColor(const Vec4& color) { m_GPUData.color = color; }

        // GPU Data Access
        const GPUMaterialData& GetGPUData() const { return m_GPUData; }
        void UpdateGPUData(); // Updates m_GPUData from internal state/maps

        // Dirty tracking
        bool IsGpuDirty() const { return m_GpuDirty; }
        bool NeedsSave()  const { return m_NeedsSave; }
        void MarkDirty()        { m_GpuDirty = true; m_NeedsSave = true; }
        void ClearGpuDirty()    { m_GpuDirty = false; }
        void ClearNeedsSave()   { m_NeedsSave = false; }

        // Serialization/Deserialization
        void Serialize(nlohmann::json& json) const;
        void Deserialize(const nlohmann::json& json);

        static const char* ToString(MapType type);

    private:
        bool SetUniformData(const std::string& name, const void* data, uint32_t size);
        bool GetUniformData(const std::string& name, void* outData, uint32_t size) const;
        void InitializeStorage();

        UUID m_ShaderUUID;
        std::vector<uint8_t> m_UniformStorage;
        // Temporary storage for deserialization if shader is not loaded yet
        nlohmann::json m_CachedUniformJSON;

        std::vector<MapInfo> m_Maps;
        
        // Cached GPU Data
        GPUMaterialData m_GPUData;

        RenderMode m_RenderMode = RenderMode::Opaque;
        BlendFactor m_BlendSrc = BlendFactor::SrcAlpha;
        BlendFactor m_BlendDst = BlendFactor::OneMinusSrcAlpha;
        float m_AlphaCutoff = 0.5f;
        CullMode m_CullMode = CullMode::Back;
        bool m_AlphaFromDiffuse = false;
        bool m_GpuDirty  = false;
        bool m_NeedsSave = false;
    };

    inline std::ostream& operator<<(std::ostream& os, const MapType type) {
        return os << Material::ToString(type);
    }
}
