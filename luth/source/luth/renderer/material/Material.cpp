#include "luthpch.h"
#include "luth/renderer/material/Material.h"
#include "luth/renderer/backend/vulkan/VulkanContext.h"

namespace Luth
{
    void Material::SetShader(const UUID& uuid)
    {
        m_ShaderUUID = uuid;
        InitializeStorage();
    }

    void Material::UpdateGPUData()
    {
        // Slot 0 is the reserved null (1x1 white) texture; BindlessOrNull coerces the
        // "not registered" sentinel returned by VKTexture::GetBindlessIndex() back to 0
        // so the SSBO never carries an out-of-range value.
        auto GetIndex = [&](MapType type) -> u32 {
            auto tex = GetTextureByType(type);
            return tex ? BindlessOrNull(tex->GetBindlessIndex()) : 0u;
        };

        // MapType::Metalness slot maps to metalRoughIndex (glTF packs metal+rough into one texture).
        m_GPUData.diffuseIndex     = GetIndex(MapType::Diffuse);
        m_GPUData.normalIndex      = GetIndex(MapType::Normal);
        m_GPUData.metalRoughIndex  = GetIndex(MapType::Metalness);
        m_GPUData.occlusionIndex   = GetIndex(MapType::Occlusion);
        m_GPUData.emissiveIndex    = GetIndex(MapType::Emissive);
        m_GPUData.alphaIndex       = GetIndex(MapType::Alpha);
        m_GPUData.specularIndex    = GetIndex(MapType::Specular);
        m_GPUData.thicknessIndex   = GetIndex(MapType::Thickness);

        // metalness/roughness are direct GPUData fields (set via accessors / deserialize); the legacy
        // u_* uniform channel never reached the GPU (no Set-1 block in pbr). alphaCutoff stays derived.
        m_GPUData.alphaCutoff = (m_RenderMode == RenderMode::Cutout) ? m_AlphaCutoff : 0.0f;

        // Flags layout documented on GPUMaterialData. Existing bits 0-4 unchanged for byte-identical
        // shader behavior; bits 5-7 land the new HAS_* signals; UV indices shift to bits 16-23.
        m_GPUData.flags = 0;
        auto SetHas = [&](MapType type, u32 bit) {
            if (GetTextureByType(type) && IsUseMapEnabled(type))
                m_GPUData.flags |= (1u << bit);
        };
        SetHas(MapType::Normal,    0);
        SetHas(MapType::Metalness, 1);
        SetHas(MapType::Occlusion, 2);
        SetHas(MapType::Diffuse,   3);
        SetHas(MapType::Emissive,  4);
        SetHas(MapType::Alpha,     5);
        SetHas(MapType::Specular,  6);
        SetHas(MapType::Thickness, 7);

        auto PackUV = [&](MapType type, u32 bitOffset) {
            auto idx = GetUVIndex(type);
            if (idx.has_value())
                m_GPUData.flags |= ((idx.value() & 0x3u) << bitOffset);
        };
        PackUV(MapType::Diffuse,   16);
        PackUV(MapType::Normal,    18);
        PackUV(MapType::Metalness, 20);
        PackUV(MapType::Occlusion, 22);
    }

    void Material::Serialize(nlohmann::json& json) const
    {
        json["shader"] = m_ShaderUUID.ToString();

        json["render_mode"] = static_cast<int>(m_RenderMode);
        json["alpha_cutoff"] = m_AlphaCutoff;
        json["blend_src"] = static_cast<int>(m_BlendSrc);
        json["blend_dst"] = static_cast<int>(m_BlendDst);
        json["alpha_from_diffuse"] = static_cast<int>(m_AlphaFromDiffuse);
        json["cull_mode"] = static_cast<int>(m_CullMode);

        // Serialize color
        json["color"] = { m_GPUData.color.r, m_GPUData.color.g, m_GPUData.color.b, m_GPUData.color.a };

        // Serialize emissive (rgb = linear factor, a = HDR strength) — direct GPUData field like color.
        json["emissive"] = { m_GPUData.emissive.r, m_GPUData.emissive.g,
                             m_GPUData.emissive.b, m_GPUData.emissive.a };

        // Serialize metalness/roughness — direct GPUData fields (the u_* uniform channel is dead).
        json["metalness"] = m_GPUData.metalness;
        json["roughness"] = m_GPUData.roughness;

        // Serialize Uniforms
        nlohmann::json uniformsJson;
        auto shader = GetShader();
        if (shader && !m_UniformStorage.empty())
        {
            for (const auto& [bufferName, buffer] : shader->GetBuffers())
            {
                for (const auto& [name, uniform] : buffer.Uniforms)
                {
                    if (uniform.Offset + uniform.Size > m_UniformStorage.size()) continue;
                    
                    const uint8_t* ptr = m_UniformStorage.data() + uniform.Offset;
                    
                    switch (uniform.Type)
                    {
                        case ShaderDataType::Float:  uniformsJson[name] = *(float*)ptr; break;
                        case ShaderDataType::Float2: uniformsJson[name] = { ((float*)ptr)[0], ((float*)ptr)[1] }; break;
                        case ShaderDataType::Float3: uniformsJson[name] = { ((float*)ptr)[0], ((float*)ptr)[1], ((float*)ptr)[2] }; break;
                        case ShaderDataType::Float4: uniformsJson[name] = { ((float*)ptr)[0], ((float*)ptr)[1], ((float*)ptr)[2], ((float*)ptr)[3] }; break;
                        case ShaderDataType::Int:    uniformsJson[name] = *(int*)ptr; break;
                        case ShaderDataType::Bool:   uniformsJson[name] = *(bool*)ptr; break;
                        default: break;
                    }
                }
            }
        }
        else if (!m_CachedUniformJSON.empty())
        {
            uniformsJson = m_CachedUniformJSON;
        }
        json["uniforms"] = uniformsJson;

        json["textures"] = nlohmann::json::array();
        for (const auto& tex : m_Maps) {
            nlohmann::json texJson;
            texJson["type"] = static_cast<int>(tex.type);
            texJson["uuid"] = tex.Uuid.ToString();
            texJson["uv"] = tex.uvIndex;
            texJson["useTexture"] = tex.useTexture;
            json["textures"].push_back(texJson);
        }
    }

    void Material::Deserialize(const nlohmann::json& json)
    {
        m_ShaderUUID = UUID::FromString(json["shader"].get<std::string>());
        
        if (json.contains("uniforms"))
            m_CachedUniformJSON = json["uniforms"];

        InitializeStorage();

        m_RenderMode = static_cast<RenderMode>(json.value("render_mode", 0));
        m_AlphaCutoff = json.value("alpha_cutoff", 0.5f);
        m_BlendSrc = static_cast<BlendFactor>(json.value("blend_src",
            static_cast<int>(BlendFactor::SrcAlpha)));
        m_BlendDst = static_cast<BlendFactor>(json.value("blend_dst",
            static_cast<int>(BlendFactor::OneMinusSrcAlpha)));
        m_AlphaFromDiffuse = static_cast<bool>(json.value("alpha_from_diffuse", 0));
        m_CullMode = static_cast<CullMode>(json.value("cull_mode", static_cast<int>(CullMode::Back)));

        if (json.contains("color") && json["color"].is_array() && json["color"].size() == 4)
            m_GPUData.color = Vec4(json["color"][0], json["color"][1], json["color"][2], json["color"][3]);
        else if (m_CachedUniformJSON.is_object() && m_CachedUniformJSON.contains("u_AlbedoColor")
                 && m_CachedUniformJSON["u_AlbedoColor"].is_array() && m_CachedUniformJSON["u_AlbedoColor"].size() == 4)
        {
            // Legacy import: base color landed in the dead u_* uniform channel. Recover it.
            const auto& c = m_CachedUniformJSON["u_AlbedoColor"];
            m_GPUData.color = Vec4(c[0], c[1], c[2], c[3]);
        }

        // Metalness/roughness — direct fields; fall back to the legacy (dead) u_* uniform JSON so
        // materials imported before these became direct fields recover their factors.
        f32 legacyMetal = m_GPUData.metalness, legacyRough = m_GPUData.roughness;
        if (m_CachedUniformJSON.is_object())
        {
            legacyMetal = m_CachedUniformJSON.value("u_Metalness", legacyMetal);
            legacyRough = m_CachedUniformJSON.value("u_Roughness", legacyRough);
        }
        m_GPUData.metalness = json.value("metalness", legacyMetal);
        m_GPUData.roughness = json.value("roughness", legacyRough);

        m_Maps.clear();
        for (const auto& texJson : json["textures"]) {
            MapInfo tex;
            tex.type = static_cast<MapType>(texJson["type"].get<int>());
            tex.Uuid = UUID::FromString(texJson["uuid"].get<std::string>());
            tex.uvIndex = texJson["uv"].get<u32>();
            tex.useTexture = static_cast<bool>(texJson.value("useTexture", 0));
            m_Maps.push_back(tex);
        }

        // Emissive factor (rgb) + HDR strength (a). Direct GPUData field, mirrors color.
        if (json.contains("emissive") && json["emissive"].is_array() && json["emissive"].size() == 4)
        {
            m_GPUData.emissive = Vec4(json["emissive"][0], json["emissive"][1],
                                      json["emissive"][2], json["emissive"][3]);
        }
        else
        {
            // Migration for files predating the emissive field. Preserve the prior "emissive texture
            // emits at full" behavior so existing emissive-textured assets don't go dark in the RT
            // path; default to no emission otherwise. Gated on key-absence only — never overrides a
            // deliberate factor from a newer save (those always carry the "emissive" key).
            bool hasEmissiveTex = false;
            for (const auto& m : m_Maps)
                if (m.type == MapType::Emissive && m.Uuid.IsValid()) { hasEmissiveTex = true; break; }
            m_GPUData.emissive = hasEmissiveTex ? Vec4(1.0f, 1.0f, 1.0f, 1.0f)
                                                : Vec4(0.0f, 0.0f, 0.0f, 1.0f);
        }
    }

    void Material::InitializeStorage()
    {
        auto shader = GetShader();
        if (!shader) return;

        const ShaderBuffer* targetBuffer = nullptr;
        for (const auto& [name, buffer] : shader->GetBuffers())
        {
            if (buffer.Set == 1) 
            {
                targetBuffer = &buffer;
                break;
            }
        }

        if (targetBuffer)
        {
            if (m_UniformStorage.size() != targetBuffer->Size)
            {
                m_UniformStorage.resize(targetBuffer->Size);
                memset(m_UniformStorage.data(), 0, m_UniformStorage.size());
            }

            if (!m_CachedUniformJSON.empty())
            {
                for (const auto& [name, uniform] : targetBuffer->Uniforms)
                {
                    if (m_CachedUniformJSON.contains(name))
                    {
                        auto& jVal = m_CachedUniformJSON[name];
                        void* ptr = m_UniformStorage.data() + uniform.Offset;

                        switch (uniform.Type)
                        {
                            case ShaderDataType::Float:  *(float*)ptr = jVal.get<float>(); break;
                            case ShaderDataType::Float2: *(Vec2*)ptr = Vec2(jVal[0], jVal[1]); break;
                            case ShaderDataType::Float3: *(Vec3*)ptr = Vec3(jVal[0], jVal[1], jVal[2]); break;
                            case ShaderDataType::Float4: *(Vec4*)ptr = Vec4(jVal[0], jVal[1], jVal[2], jVal[3]); break;
                            case ShaderDataType::Int:    *(int*)ptr = jVal.get<int>(); break;
                            case ShaderDataType::Bool:   *(bool*)ptr = jVal.get<bool>(); break;
                            default: break;
                        }
                    }
                }
                m_CachedUniformJSON.clear();
            }
        }
    }

    bool Material::SetUniformData(const std::string& name, const void* data, uint32_t size)
    {
        if (m_UniformStorage.empty()) InitializeStorage();
        if (m_UniformStorage.empty()) return false;

        auto shader = GetShader();
        if (!shader) return false;

        for (const auto& [buffName, buffer] : shader->GetBuffers())
        {
            if (buffer.Set != 1) continue;
            
            auto it = buffer.Uniforms.find(name);
            if (it != buffer.Uniforms.end())
            {
                const auto& uniform = it->second;
                if (uniform.Size == size)
                {
                    memcpy(m_UniformStorage.data() + uniform.Offset, data, size);
                    return true;
                }
            }
        }
        return false;
    }

    bool Material::GetUniformData(const std::string& name, void* outData, uint32_t size) const
    {
        if (m_UniformStorage.empty()) return false;
        auto shader = GetShader();
        if (!shader) return false;

        for (const auto& [buffName, buffer] : shader->GetBuffers()) {
            if (buffer.Set != 1) continue;
            auto it = buffer.Uniforms.find(name);
            if (it != buffer.Uniforms.end() && it->second.Size == size) {
                memcpy(outData, m_UniformStorage.data() + it->second.Offset, size);
                return true;
            }
        }
        return false;
    }

    const char* Material::ToString(MapType type) {
        switch (type) {
            case MapType::Diffuse:   return "Diffuse";
            case MapType::Alpha:     return "Alpha";
            case MapType::Normal:    return "Normal";
            case MapType::Emissive:  return "Emissive";
            case MapType::Metalness: return "Metalness";
            case MapType::Roughness: return "Roughness";
            case MapType::Specular:  return "Specular";
            case MapType::Occlusion:  return "Occlusion";
            default: return "Unknown";
        }
    }
}
