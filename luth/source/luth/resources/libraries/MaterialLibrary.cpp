#include "luthpch.h"
#include "luth/resources/libraries/MaterialLibrary.h"
#include "luth/resources/libraries/ShaderLibrary.h"
#include "luth/resources/ResourceDB.h"

namespace Luth
{
    std::shared_mutex MaterialLibrary::s_Mutex;
    std::unordered_map<UUID, std::shared_ptr<Material>, UUIDHash> MaterialLibrary::s_Materials;

    void MaterialLibrary::Init()
    {
        //std::unique_lock lock(s_Mutex);
        LH_CORE_INFO("Initialized Material Library");
    }

    void MaterialLibrary::Shutdown()
    {
        std::unique_lock lock(s_Mutex);
        s_Materials.clear();
        LH_CORE_INFO("Cleared Material Library");
    }

    std::shared_ptr<Material> MaterialLibrary::CreateNew()
    {
        auto material = std::make_shared<Material>();
        material->SetUUID(UUID());  // Generate new UUID
        return material;
    }

    std::shared_ptr<Material> MaterialLibrary::LoadOrGet(const fs::path& path)
    {
        UUID materialUUID = ResourceDB::GetUuidForPath(path);

        if (auto existing = Get(materialUUID)) {
            return existing;
        }

        // Load from file
        auto material = std::make_shared<Material>();
        material->SetUUID(materialUUID);
        material->SetName(path.filename().stem().string());

        // Load serialized data
        std::ifstream file(path);
        nlohmann::json json;
        file >> json;
        material->Deserialize(json);

        // Store in library
        std::unique_lock lock(s_Mutex);
        s_Materials[materialUUID] = material;

        return material;
    }

    std::shared_ptr<Material> MaterialLibrary::Get(const UUID& uuid)
    {
        std::shared_lock lock(s_Mutex);
        auto it = s_Materials.find(uuid);
        if (it != s_Materials.end()) {
            if (auto material = it->second) {
                return material;
            }
        }
        return nullptr;
    }

    std::unordered_map<UUID, std::shared_ptr<Material>, UUIDHash> MaterialLibrary::GetAllMaterials()
    {
        return s_Materials;
    }

    bool MaterialLibrary::Save(const UUID& materialUUID)
    {
        if (auto material = Get(materialUUID)) {
            auto path = ResourceDB::ResolveUuid(materialUUID);
            if (path.empty()) return false;

            nlohmann::json json;
            material->Serialize(json);

            std::ofstream file(path);
            file << json.dump(4);
            return true;
        }
        return false;
    }

    void MaterialLibrary::Reload(const UUID& materialUUID)
    {
        auto path = ResourceDB::ResolveUuid(materialUUID);
        if (path.empty()) return;

        if (auto material = Get(materialUUID)) {
            // Preserve runtime modifications
            Material temp = *material;

            // Reload from disk
            std::ifstream file(path);
            nlohmann::json json;
            file >> json;
            material->Deserialize(json);

            // Restore UUID and name
            material->SetUUID(materialUUID);
        }
    }
}
