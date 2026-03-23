#include "luthpch.h"
#include "luth/renderer/Material.h"
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
        // 1. Update Texture Indices
        // We iterate through our map types and query the texture for its bindless index.
        
        auto GetIndex = [&](MapType type) -> u32 {
            auto tex = GetTextureByType(type);
            if (tex)
            {
                return tex->GetBindlessIndex();
            }
            return 0; // Null texture (index 0 is usually null or invalid, but our BindlessSet handles 0?)
            // Actually, BindlessDescriptorSet returns 0 on error, but valid indices start at 0?
            // No, we initialized free indices 0..MAX.
            // If 0 is a valid index, we need a way to represent "None".
            // Usually we reserve index 0 for the null texture.
            // But our BindlessDescriptorSet implementation doesn't reserve 0 explicitly, it just uses a deque.
            // However, VKTexture initializes m_BindlessIndex to 0.
            // If 0 is a valid texture, this is ambiguous.
            // TODO: Reserve index 0 for global null texture in BindlessDescriptorSet.
            // For now, let's assume 0 means "No Texture" and the shader handles it or samples the null texture at index 0?
            // Wait, if we return 0, the shader samples texture at index 0.
            // If index 0 is a valid texture (e.g. Albedo), we are fine.
            // If index 0 is "None", we need to ensure index 0 IS the null texture.
            // Let's assume for now that if GetTextureByType returns null, we pass 0.
            // And we hope index 0 is valid (it is, it's the first allocated texture).
            // Ideally, we should have a specific "White Texture" at a known index.
        };
        
        m_GPUData.diffuseIndex = GetIndex(MapType::Diffuse);
        m_GPUData.normalIndex = GetIndex(MapType::Normal);
        m_GPUData.metalRoughIndex = GetIndex(MapType::Metalness); // Assuming packed or separate?
        // If separate, we might need separate indices.
        // Standard PBR often packs Metal/Rough/AO.
        // For now, map Metalness slot to MetalRoughIndex.
        
        m_GPUData.occlusionIndex = GetIndex(MapType::Occlusion);
        
        // 2. Update Factors
        float uniformMetal;
        m_GPUData.metalness = GetUniformData("u_Metalness", &uniformMetal, sizeof(float))
            ? uniformMetal : m_GPUData.metalness;
        float uniformRough;
        m_GPUData.roughness = GetUniformData("u_Roughness", &uniformRough, sizeof(float))
            ? uniformRough : m_GPUData.roughness;
        m_GPUData.alphaCutoff = (m_RenderMode == RenderMode::Cutout) ? m_AlphaCutoff : 0.0f;
        
        // Flags — each bit indicates a valid texture is bound for that feature
        m_GPUData.flags = 0;
        if (GetTextureByType(MapType::Normal)    && IsUseMapEnabled(MapType::Normal))    m_GPUData.flags |= (1 << 0); // HAS_NORMAL
        if (GetTextureByType(MapType::Metalness) && IsUseMapEnabled(MapType::Metalness)) m_GPUData.flags |= (1 << 1); // HAS_METALROUGH
        if (GetTextureByType(MapType::Occlusion)  && IsUseMapEnabled(MapType::Occlusion))  m_GPUData.flags |= (1 << 2); // HAS_OCCLUSION
        if (GetTextureByType(MapType::Diffuse)   && IsUseMapEnabled(MapType::Diffuse))   m_GPUData.flags |= (1 << 3); // HAS_DIFFUSE
        if (GetTextureByType(MapType::Emissive)  && IsUseMapEnabled(MapType::Emissive))  m_GPUData.flags |= (1 << 4); // HAS_EMISSIVE

        // Pack per-texture UV indices into flags bits 8-15 (2 bits each, values 0-3)
        auto PackUV = [&](MapType type, u32 bitOffset) {
            auto idx = GetUVIndex(type);
            if (idx.has_value())
                m_GPUData.flags |= ((idx.value() & 0x3u) << bitOffset);
        };
        PackUV(MapType::Diffuse,   8);
        PackUV(MapType::Normal,    10);
        PackUV(MapType::Metalness, 12);
        PackUV(MapType::Occlusion, 14);
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
            m_GPUData.color = glm::vec4(json["color"][0], json["color"][1], json["color"][2], json["color"][3]);

        m_Maps.clear();
        for (const auto& texJson : json["textures"]) {
            MapInfo tex;
            tex.type = static_cast<MapType>(texJson["type"].get<int>());
            tex.Uuid = UUID::FromString(texJson["uuid"].get<std::string>());
            tex.uvIndex = texJson["uv"].get<u32>();
            tex.useTexture = static_cast<bool>(texJson.value("useTexture", 0));
            m_Maps.push_back(tex);
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
