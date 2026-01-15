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
        // We need to bind textures to the global heap and get their indices.
        // This should ideally happen when textures are loaded or assigned, but doing it here ensures sync.
        // Note: BindTexture is thread-safe.
        
        auto& bindless = VulkanContext::Get().GetBindlessSet();
        
        // Helper to bind and get index
        auto Bind = [&](MapType type) -> u32 {
            auto tex = GetTextureByType(type);
            if (tex)
            {
                // We need the VKTexture to get view/sampler
                // Assuming Texture is VKTexture in Vulkan mode
                // TODO: Add virtual GetImageView/Sampler to Texture interface or cast
                // For now, we assume VKTexture.
                // But Texture.h doesn't expose Vulkan types.
                // We need to cast.
                // This requires including VKTexture.h which couples Material to Vulkan.
                // Ideally, Texture should have a "GetBindlessIndex()" method.
                // But Bindless is a Vulkan concept.
                
                // Let's assume Texture has a void* GetNativeHandle() or similar.
                // Or we cast.
                // Since we are in Phase 7, we can assume Vulkan backend.
                // But Material.cpp is generic.
                // We should move this logic to a backend-specific update?
                // Or make Texture expose BindlessIndex.
                
                // For now, let's just use 0 (Null Texture) if we can't cast easily without including backend headers.
                // Wait, we can include VulkanTexture.h here.
                return 0; // Placeholder until we include VKTexture
            }
            return 0;
        };
        
        // We need to include VulkanTexture.h to cast.
        // But let's do it properly.
        // Texture should have `u32 GetBindlessIndex()` which returns 0 if not supported.
        // VKTexture will implement it by calling BindlessDescriptorSet::BindTexture on creation/load.
        
        // Actually, textures should be bound when loaded.
        // So we just query the texture for its index.
        
        // For now, let's just update the factors.
        m_GPUData.color = Get<glm::vec4>("u_Color", glm::vec4(1.0f));
        m_GPUData.metalness = Get<float>("u_Metalness", 0.0f);
        m_GPUData.roughness = Get<float>("u_Roughness", 0.5f);
        m_GPUData.alphaCutoff = m_AlphaCutoff;
        
        // Map texture types to GPU struct fields
        // We need to iterate m_Maps and find indices.
        // Since we don't have the indices yet (Texture doesn't expose them), we leave them 0.
        // The next step in Phase 7 is to make Textures bind themselves.
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
            case MapType::Oclusion:  return "Oclusion";
            default: return "Unknown";
        }
    }
}
