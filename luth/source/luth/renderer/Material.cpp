#include "luthpch.h"
#include "luth/renderer/Material.h"

namespace Luth
{
    void Material::Serialize(nlohmann::json& json) const
    {
        json["shader"] = m_ShaderUUID.ToString();

        json["render_mode"] = static_cast<int>(m_RenderMode);
        json["alpha_cutoff"] = m_AlphaCutoff;
        json["blend_src"] = static_cast<int>(m_BlendSrc);
        json["blend_dst"] = static_cast<int>(m_BlendDst);

        json["textures"] = nlohmann::json::array();
        for (const auto& tex : m_Textures) {
            nlohmann::json texJson;
            texJson["type"] = static_cast<int>(tex.type);
            texJson["uuid"] = tex.Uuid.ToString();
            texJson["uv"] = tex.uvIndex;
            json["textures"].push_back(texJson);
        }
    }

    void Material::Deserialize(const nlohmann::json& json)
    {
        UUID::FromString(json["shader"].get<std::string>(), m_ShaderUUID);

        m_RenderMode = static_cast<RendererAPI::RenderMode>(json.value("render_mode", 0));
        m_AlphaCutoff = json.value("alpha_cutoff", 0.5f);
        m_BlendSrc = static_cast<RendererAPI::BlendFactor>(json.value("blend_src",
            static_cast<int>(RendererAPI::BlendFactor::SrcAlpha)));
        m_BlendDst = static_cast<RendererAPI::BlendFactor>(json.value("blend_dst",
            static_cast<int>(RendererAPI::BlendFactor::OneMinusSrcAlpha)));

        m_Textures.clear();
        for (const auto& texJson : json["textures"]) {
            TextureInfo tex;
            tex.type = static_cast<TextureType>(texJson["type"].get<int>());
            UUID::FromString(texJson["uuid"].get<std::string>(), tex.Uuid);
            tex.uvIndex = texJson["uv"].get<u32>();
            m_Textures.push_back(tex);
        }
    }

    const char* Material::ToString(TextureType type) {
        switch (type) {
            case TextureType::Diffuse:   return "Diffuse";
            case TextureType::Alpha:     return "Alpha";
            case TextureType::Normal:    return "Normal";
            case TextureType::Emissive:  return "Emissive";
            case TextureType::Metalness: return "Metalness";
            case TextureType::Roughness: return "Roughness";
            case TextureType::Specular:  return "Specular";
            case TextureType::Oclusion:  return "Oclusion";
            default: return "Unknown";
        }
    }
}
