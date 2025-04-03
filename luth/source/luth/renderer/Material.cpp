#include "luthpch.h"
#include "luth/renderer/Material.h"

namespace Luth
{
    void Material::Serialize(nlohmann::json& json) const
    {
        json["shader"] = m_ShaderUUID.ToString();

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

        m_Textures.clear();
        for (const auto& texJson : json["textures"]) {
            TextureInfo tex;
            tex.type = static_cast<TextureType>(texJson["type"].get<int>());
            UUID::FromString(texJson["uuid"].get<std::string>(), tex.Uuid);
            tex.uvIndex = texJson["uv"].get<u32>();
            m_Textures.push_back(tex);
        }
    }
}
