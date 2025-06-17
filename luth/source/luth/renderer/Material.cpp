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
        json["alpha_from_diffuse"] = static_cast<int>(m_AlphaFromDiffuse);

        json["color"] = { m_Color.r, m_Color.g, m_Color.b, m_Color.a };
        json["alpha"] = m_Alpha;
        json["metal"] = m_Metal;
        json["rough"] = m_Rough;
        json["emissive"] = { m_Emissive.r, m_Emissive.g, m_Emissive.b };
        json["is_gloss"] = static_cast<int>(m_IsGloss);
        json["is_single_channel"] = static_cast<int>(m_IsSingleChannel);

        json["subsurface"] = {
            {"color", {m_Subsurface.color.r, m_Subsurface.color.g, m_Subsurface.color.b}},
            {"strength", m_Subsurface.strength},
            {"thickness_scale", m_Subsurface.thicknessScale}
        };

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
        UUID::FromString(json["shader"].get<std::string>(), m_ShaderUUID);

        m_RenderMode = static_cast<RendererAPI::RenderMode>(json.value("render_mode", 0));
        m_AlphaCutoff = json.value("alpha_cutoff", 0.5f);
        m_BlendSrc = static_cast<RendererAPI::BlendFactor>(json.value("blend_src",
            static_cast<int>(RendererAPI::BlendFactor::SrcAlpha)));
        m_BlendDst = static_cast<RendererAPI::BlendFactor>(json.value("blend_dst",
            static_cast<int>(RendererAPI::BlendFactor::OneMinusSrcAlpha)));
        m_AlphaFromDiffuse = static_cast<bool>(json.value("alpha_from_diffuse", 0));

        if (json.contains("color")) {
            auto& jc = json["color"];
            m_Color = glm::vec4(jc[0].get<float>(), jc[1].get<float>(),
                jc[2].get<float>(), jc[3].get<float>());
        }

        m_Alpha = json.value("alpha", 1.0f);
        m_Metal = json.value("metal", 0.0f);
        m_Rough = json.value("rough", 1.0f);

        if (json.contains("emissive")) {
            auto& je = json["emissive"];
            m_Emissive = glm::vec3(je[0].get<float>(), je[1].get<float>(), je[2].get<float>());
        }
        else {
            m_Emissive = glm::vec3(0.0f);
        }

        m_IsGloss = static_cast<bool>(json.value("is_gloss", 0));
        m_IsSingleChannel = static_cast<bool>(json.value("is_single_channel", 0));

        if (json.contains("subsurface")) {
            const auto& subsurfaceJson = json["subsurface"];

            if (subsurfaceJson.contains("color")) {
                auto& jc = subsurfaceJson["color"];
                m_Subsurface.color = glm::vec3(jc[0].get<float>(), jc[1].get<float>(), jc[2].get<float>());
            }
            m_Subsurface.strength = subsurfaceJson.value("strength", 1.0f);
            m_Subsurface.thicknessScale = subsurfaceJson.value("thickness_scale", 1.0f);
        }
        else {
            m_Subsurface = Subsurface{};
        }

        m_Maps.clear();
        for (const auto& texJson : json["textures"]) {
            MapInfo tex;
            tex.type = static_cast<MapType>(texJson["type"].get<int>());
            UUID::FromString(texJson["uuid"].get<std::string>(), tex.Uuid);
            tex.uvIndex = texJson["uv"].get<u32>();
            tex.useTexture = static_cast<bool>(texJson.value("useTexture", 0));
            m_Maps.push_back(tex);
        }
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
