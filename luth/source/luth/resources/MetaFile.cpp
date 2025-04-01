#include "luthpch.h"
#include "luth/resources/MetaFile.h"
#include "luth/resources/ResourceDB.h"

namespace Luth
{
    UUID MetaFile::Create(const fs::path& path, ResourceType type)
    {
        UUID newUuid;
        MetaFile meta(newUuid);

        SetDefaultTypeSettings(type, meta);

        const auto metaPath = path.string() + ".meta";
        if (!meta.Save(metaPath)) {
            throw std::runtime_error("Failed to create .meta file");
        }

        ResourceDB::RegisterAsset(path, newUuid);
        return newUuid;
    }

    void MetaFile::SetDefaultTypeSettings(ResourceType type, MetaFile& meta)
    {
        auto& settings = meta.GetTypeSettings();

        switch (type) {
            case ResourceType::Texture:
                settings["generate_mipmaps"] = true;
                settings["compression_format"] = "BC7";
                settings["srgb"] = true;
                break;

            case ResourceType::Model:
                settings["import_normals"] = true;
                settings["import_tangents"] = false;
                settings["optimize_mesh"] = true;
                break;

            case ResourceType::Material:
                settings["shader"] = "Lit";
                settings["blend_mode"] = "Opaque";
                break;

            case ResourceType::Shader:
                settings["hot_reload"] = true;
                settings["optimization_level"] = 3;
                break;

            default:
                break;
        }
    }

    bool MetaFile::Load(const fs::path& metaPath)
    {
        std::ifstream file(metaPath);
        if (!file.is_open()) return false;

        try {
            nlohmann::json json;
            file >> json;

            // Validate format version
            if (json["version"] != FORMAT_VERSION) return false;

            // Parse UUID from hex string
            std::string uuidStr = json["uuid"].get<std::string>();
            uint64_t uuidValue;
            std::stringstream ss;
            ss << std::hex << uuidStr;
            ss >> uuidValue;
            m_UUID = UUID(uuidValue);

            // Parse dependencies
            m_Dependencies.clear();
            for (const auto& dep : json["dependencies"]) {
                std::string depStr = dep.get<std::string>();
                uint64_t depValue;
                std::stringstream depSS;
                depSS << std::hex << depStr;
                depSS >> depValue;
                m_Dependencies.emplace_back(depValue);
            }

            // Load type-specific settings
            m_TypeSettings = json["type_settings"];

            return true;
        }
        catch (...) {
            return false;
        }
    }

    bool MetaFile::Save(const fs::path& metaPath) const
    {
        nlohmann::json json;
        json["version"] = FORMAT_VERSION;

        // Convert UUID to hex string
        std::stringstream uuidSS;
        uuidSS << std::hex << std::setw(16) << std::setfill('0') << static_cast<uint64_t>(m_UUID);
        json["uuid"] = uuidSS.str();

        // Serialize dependencies
        json["dependencies"] = nlohmann::json::array();
        for (const auto& dep : m_Dependencies) {
            std::stringstream depSS;
            depSS << std::hex << std::setw(16) << std::setfill('0') << static_cast<uint64_t>(dep);
            json["dependencies"].push_back(depSS.str());
        }

        // Type-specific settings
        json["type_settings"] = m_TypeSettings;

        std::ofstream file(metaPath);
        if (!file.is_open()) return false;

        file << json.dump(4);
        return true;
    }

    void MetaFile::AddDependency(const UUID& dependency)
    {
        if (std::find(m_Dependencies.begin(), m_Dependencies.end(), dependency) == m_Dependencies.end()) {
            m_Dependencies.push_back(dependency);
        }
    }
}
