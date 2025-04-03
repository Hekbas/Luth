#include "luthpch.h"
#include "luth/resources/libraries/ModelLibrary.h"
#include "luth/resources/ResourceDB.h"

namespace Luth
{
    std::shared_mutex ModelLibrary::s_Mutex;
    std::unordered_map<UUID, ModelLibrary::ModelRecord, UUIDHash> ModelLibrary::s_Models;

    void ModelLibrary::Init()
    {
        std::unique_lock lock(s_Mutex);
        LH_CORE_INFO("Initialized Model Library");
    }

    void ModelLibrary::Shutdown()
    {
        std::unique_lock lock(s_Mutex);
        s_Models.clear();
        LH_CORE_INFO("Cleared Model Library");
    }

    bool ModelLibrary::Add(std::shared_ptr<Model> model)
    {
        if (!model) {
            LH_CORE_ERROR("Attempted to add null Model");
            return false;
        }

        UUID uuid = model->GetUUID();
        std::unique_lock lock(s_Mutex);
        auto [it, inserted] = s_Models.try_emplace(uuid, ModelRecord{ model, {} });

        if (!inserted) {
            LH_CORE_WARN("Model with UUID {0} already exists! Overwriting...", uuid.ToString());
            it->second = { model, {} };
        }
        return true;
    }

    bool ModelLibrary::Remove(const UUID& uuid)
    {
        std::unique_lock lock(s_Mutex);
        return s_Models.erase(uuid) > 0;
    }

    bool ModelLibrary::Contains(const UUID& uuid)
    {
        std::shared_lock lock(s_Mutex);
        return s_Models.find(uuid) != s_Models.end();
    }

    std::shared_ptr<Model> ModelLibrary::Get(const UUID& uuid)
    {
        std::shared_lock lock(s_Mutex);
        auto it = s_Models.find(uuid);
        return it != s_Models.end() ? it->second.Model : nullptr;
    }

    std::unordered_map<UUID, ModelLibrary::ModelRecord, UUIDHash> ModelLibrary::GetAllModels()
    {
        return s_Models;
    }

    std::vector<UUID> ModelLibrary::GetAllUuids()
    {
        std::shared_lock lock(s_Mutex);
        std::vector<UUID> uuids;
        uuids.reserve(s_Models.size());
        for (const auto& [uuid, _] : s_Models)
            uuids.push_back(uuid);
        return uuids;
    }

    std::shared_ptr<Model> ModelLibrary::Load(const fs::path& path)
    {
        if (!fs::exists(path)) {
            LH_CORE_ERROR("Model file not found: {0}", path.string());
            return nullptr;
        }

        UUID uuid = ResourceDB::GetUuidForPath(path);
        if (auto existing = Get(uuid)) {
            LH_CORE_INFO("Model already loaded: {0}", uuid.ToString());
            return existing;
        }

        auto model = std::make_shared<Model>(path);
        if (!model || model->GetMeshes().empty()) {
            LH_CORE_ERROR("Failed to load Model from {0}", path.string());
            return nullptr;
        }

        model->SetUUID(uuid);
        auto modTime = fs::last_write_time(path);

        std::unique_lock lock(s_Mutex);
        s_Models[uuid] = { model, modTime };
        LH_CORE_TRACE("Loaded Model as {0}", uuid.ToString());
        return model;
    }

    std::shared_ptr<Model> ModelLibrary::LoadOrGet(const fs::path& path)
    {
        UUID uuid = ResourceDB::GetUuidForPath(path);
        if (auto model = Get(uuid)) {
            return model;
        }
        return Load(path);
    }

    bool ModelLibrary::Reload(const UUID& uuid)
    {
        std::unique_lock lock(s_Mutex);
        auto it = s_Models.find(uuid);
        if (it == s_Models.end()) {
            LH_CORE_WARN("Cannot reload non-existent Model {0}", uuid.ToString());
            return false;
        }

        auto path = ResourceDB::ResolveUuid(uuid);
        if (path.empty()) {
            LH_CORE_ERROR("No source path for Model {0}", uuid.ToString());
            return false;
        }

        if (!fs::exists(path)) {
            LH_CORE_ERROR("Model source file missing: {0}", path.string());
            return false;
        }

        const auto newTime = fs::last_write_time(path);
        if (newTime <= it->second.LastModified)
            return true; // Already up-to-date

        try {
            auto newModel = std::make_shared<Model>(path);
            if (!newModel || newModel->GetMeshes().empty()) {
                throw std::runtime_error("Model loading failed");
            }

            newModel->SetUUID(uuid);
            it->second = { newModel, newTime };
            LH_CORE_INFO("Successfully reloaded Model {0}", uuid.ToString());
            return true;
        }
        catch (const std::exception& e) {
            LH_CORE_ERROR("Failed to reload Model {0}: {1}", uuid.ToString(), e.what());
            return false;
        }
    }

    void ModelLibrary::ReloadAll()
    {
        std::unique_lock lock(s_Mutex);
        LH_CORE_INFO("Reloading all Models...");

        size_t successCount = 0;
        size_t failCount = 0;

        for (auto& [uuid, record] : s_Models) {
            auto path = ResourceDB::ResolveUuid(uuid);
            if (path.empty()) {
                LH_CORE_WARN("Skipping Model {0} with invalid path", uuid.ToString());
                failCount++;
                continue;
            }

            if (!fs::exists(path)) {
                LH_CORE_ERROR("Model source missing: {0}", path.string());
                failCount++;
                continue;
            }

            const auto newTime = fs::last_write_time(path);
            if (newTime <= record.LastModified)
                continue;

            try {
                auto newModel = std::make_shared<Model>(path);
                if (!newModel || newModel->GetMeshes().empty()) {
                    throw std::runtime_error("Empty Model");
                }

                newModel->SetUUID(uuid);
                record = { newModel, newTime };
                successCount++;
            }
            catch (const std::exception& e) {
                LH_CORE_ERROR("Reload failed for {0}: {1}", uuid.ToString(), e.what());
                failCount++;
            }
        }

        LH_CORE_INFO("Reloaded Models: {0} succeeded, {1} failed", successCount, failCount);
    }
}
