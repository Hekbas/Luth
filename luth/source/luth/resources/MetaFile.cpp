#include "luthpch.h"
#include "luth/resources/MetaFile.h"

namespace Luth
{
    UUID MetaFile::Create(const fs::path& path, AssetType type)
    {
        UUID newUuid;
        MetaFile meta(newUuid);

        SetDefaultTypeSettings(type, meta);

        const auto metaPath = path.string() + ".meta";
        if (!meta.Save(metaPath)) {
            throw std::runtime_error("Failed to create .meta file");
        }

        return newUuid;
    }

    void MetaFile::SetDefaultTypeSettings(AssetType type, MetaFile& meta)
    {
        auto& settings = meta.GetTypeSettings();

        switch (type) {
            case AssetType::Texture:
                settings["generate_mipmaps"] = true;
                settings["compression"] = "auto";       // auto | none | bc1 | bc4 | bc5 | bc7
                settings["compression_quality"] = 1;    // 0 fast | 1 normal | 2 high
                settings["wrap_mode"] = 0;   // TextureWrapMode::Repeat
                settings["filter_min"] = 0;  // TextureFilterMode::Linear
                settings["filter_mag"] = 0;  // TextureFilterMode::Linear
                break;

            case AssetType::Model:
                settings["import_normals"] = true;
                settings["import_tangents"] = false;
                settings["optimize_mesh"] = true;
                settings["scale_factor"] = 1.0f;
                settings["up_axis"] = -1;              // -1 = auto-detect
                settings["bake_axis_conversion"] = true;
                settings["skin_mesh_transform"] = 0;   // Auto
                settings["physics_bake"] = 0;          // None; opt-in per model
                break;

            case AssetType::Material:
                settings["shader"] = "Lit";
                settings["blend_mode"] = "Opaque";
                break;

            case AssetType::PhysicsMaterial:
                // No type-specific knobs at the .meta layer; friction/restitution/density live
                // inside the .physmat JSON itself.
                break;

            case AssetType::Shader:
                settings["hot_reload"] = true;
                settings["optimization_level"] = 3;
                break;

            // case AssetType::Directory:
            //     settings["is_folder"] = true;
            //     break;

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
            m_UUID = UUID::FromString(uuidStr);

            // Parse dependencies
            m_Dependencies.clear();
            for (const auto& dep : json["dependencies"]) {
                std::string depStr = dep.get<std::string>();
                m_Dependencies.emplace_back(UUID::FromString(depStr));
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

        json["uuid"] = m_UUID.ToString();

        // Serialize dependencies
        json["dependencies"] = nlohmann::json::array();
        for (const auto& dep : m_Dependencies) {
            json["dependencies"].push_back(dep.ToString());
        }

        // Type-specific settings
        json["type_settings"] = m_TypeSettings;

        std::ofstream file(metaPath);
        if (!file.is_open())
        {
            LH_LOG(Assets, error, "MetaFile::Save: Cannot open file for writing: {0}", metaPath.string());
            return false;
        }

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
