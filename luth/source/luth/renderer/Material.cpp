#include "luthpch.h"
#include "luth/renderer/Material.h"

namespace Luth
{
    void Material::SetShader(const UUID& uuid)
    {
        m_ShaderUUID = uuid;
        InitializeStorage();
    }

    void Material::Serialize(nlohmann::json& json) const
    {
        json["shader"] = m_ShaderUUID.ToString();

        json["render_mode"] = static_cast<int>(m_RenderMode);
        json["alpha_cutoff"] = m_AlphaCutoff;
        json["blend_src"] = static_cast<int>(m_BlendSrc);
        json["blend_dst"] = static_cast<int>(m_BlendDst);
        json["alpha_from_diffuse"] = static_cast<int>(m_AlphaFromDiffuse);

        // Serialize Uniforms
        // We need the shader to know types to serialize correctly back to JSON
        // For now, we can try to serialize based on the cached JSON or reconstruct it
        // Ideally, we iterate the shader uniforms and read from m_UniformStorage
        nlohmann::json uniformsJson;
        auto shader = GetShader();
        if (shader && !m_UniformStorage.empty())
        {
            for (const auto& [bufferName, buffer] : shader->GetBuffers())
            {
                // Assuming we only serialize the main material buffer for now
                // TODO: Handle multiple buffers if needed
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
        
        // Cache uniforms to apply when shader is loaded
        if (json.contains("uniforms"))
            m_CachedUniformJSON = json["uniforms"];

        InitializeStorage(); // Try to init if shader is already loaded

        m_RenderMode = static_cast<RenderMode>(json.value("render_mode", 0));
        m_AlphaCutoff = json.value("alpha_cutoff", 0.5f);
        m_BlendSrc = static_cast<BlendFactor>(json.value("blend_src",
            static_cast<int>(BlendFactor::SrcAlpha)));
        m_BlendDst = static_cast<BlendFactor>(json.value("blend_dst",
            static_cast<int>(BlendFactor::OneMinusSrcAlpha)));
        m_AlphaFromDiffuse = static_cast<bool>(json.value("alpha_from_diffuse", 0));

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

        // Find the material uniform buffer (Convention: "MaterialUniforms" or Set 1 Binding 0)
        // For now, we take the first buffer that is NOT global (Set 0)
        const ShaderBuffer* targetBuffer = nullptr;
        for (const auto& [name, buffer] : shader->GetBuffers())
        {
            if (buffer.Set == 1) // Convention: Material data is Set 1
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

            // Apply cached JSON values if any
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

        // Find uniform in Set 1
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
            case MapType::Oclusion:  return "Oclusion";
            default: return "Unknown";
        }
    }
}
