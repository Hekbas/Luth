#include "luthpch.h"
#include "luth/resources/ModelLibrary.h"

namespace Luth
{
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

    bool ModelLibrary::Add(const std::string& name, std::shared_ptr<Model> model)
    {
        std::unique_lock lock(s_Mutex);
        if (!model)
        {
            LH_CORE_ERROR("Attempted to add null model '{0}'", name);
            return false;
        }

        auto [it, inserted] = s_Models.try_emplace(name, ModelRecord{ model, "", {} });
        if (!inserted)
        {
            LH_CORE_WARN("Model '{0}' already exists! Overwriting...", name);
            it->second = { model, "", {} };
        }
        return true;
    }

    bool ModelLibrary::Remove(const std::string& name)
    {
        std::unique_lock lock(s_Mutex);
        return s_Models.erase(name) > 0;
    }

    std::shared_ptr<Model> ModelLibrary::Get(const std::string& name)
    {
        std::shared_lock lock(s_Mutex);
        auto it = s_Models.find(name);
        if (it == s_Models.end())
        {
            LH_CORE_ERROR("Model '{0}' not found in library", name);
            return nullptr;
        }
        return it->second.model;
    }

    bool ModelLibrary::Contains(const std::string& name)
    {
        std::shared_lock lock(s_Mutex);
        return s_Models.find(name) != s_Models.end();
    }

    std::vector<std::string> ModelLibrary::GetModelNames()
    {
        std::shared_lock lock(s_Mutex);
        std::vector<std::string> names;
        names.reserve(s_Models.size());
        for (const auto& [name, _] : s_Models)
            names.push_back(name);
        return names;
    }

    std::shared_ptr<Model> ModelLibrary::Load(const std::filesystem::path& path)
    {
        namespace fs = std::filesystem;

        if (!fs::exists(path))
        {
            LH_CORE_ERROR("Model file not found: {0}", path.string());
            return nullptr;
        }

        auto model = std::make_shared<Model>(path);
        if (!model || model->GetMeshes().empty())
        {
            LH_CORE_ERROR("Failed to load model from {0}", path.string());
            return nullptr;
        }

        const std::string name = path.stem().string();
        const auto modTime = fs::last_write_time(path);

        std::unique_lock lock(s_Mutex);
        s_Models[name] = { model, path, modTime };
        LH_CORE_INFO("Loaded model '{0}' from {1}", name, path.string());
        return model;
    }

    std::shared_ptr<Model> ModelLibrary::Load(const std::string& name, const std::filesystem::path& path)
    {
        namespace fs = std::filesystem;

        if (!fs::exists(path))
        {
            LH_CORE_ERROR("Model file not found: {0}", path.string());
            return nullptr;
        }

        auto model = std::make_shared<Model>(path);
        if (!model || model->GetMeshes().empty())
        {
            LH_CORE_ERROR("Failed to load model '{0}' from {1}", name, path.string());
            return nullptr;
        }

        const auto modTime = fs::last_write_time(path);

        std::unique_lock lock(s_Mutex);
        s_Models[name] = { model, path, modTime };
        LH_CORE_INFO("Loaded model '{0}' from {1}", name, path.string());
        return model;
    }

    std::shared_ptr<Model> ModelLibrary::LoadOrGet(const std::filesystem::path& path)
    {
        const std::string name = path.stem().string();
        std::shared_lock lock(s_Mutex);
        if (auto it = s_Models.find(name); it != s_Models.end())
            return it->second.model;
        lock.unlock();
        return Load(path);
    }

    std::shared_ptr<Model> ModelLibrary::LoadOrGet(const std::string& name, const std::filesystem::path& path)
    {
        std::shared_lock lock(s_Mutex);
        if (auto it = s_Models.find(name); it != s_Models.end())
            return it->second.model;
        lock.unlock();
        return Load(name, path);
    }

    bool ModelLibrary::Reload(const std::string& name)
    {
        std::unique_lock lock(s_Mutex);
        auto it = s_Models.find(name);
        if (it == s_Models.end())
        {
            LH_CORE_WARN("Cannot reload non-existent model '{0}'", name);
            return false;
        }

        auto& record = it->second;
        if (record.sourcePath.empty())
        {
            LH_CORE_INFO("Model '{0}' not reloadable (no source path)", name);
            return false;
        }

        namespace fs = std::filesystem;
        if (!fs::exists(record.sourcePath))
        {
            LH_CORE_ERROR("Model source file missing: {0}", record.sourcePath.string());
            return false;
        }

        const auto newTime = fs::last_write_time(record.sourcePath);
        if (newTime <= record.lastModified)
            return true; // Already up-to-date

        try {
            auto newModel = std::make_shared<Model>(record.sourcePath);
            if (!newModel || newModel->GetMeshes().empty())
                throw std::runtime_error("Model loading failed");

            record.model = newModel;
            record.lastModified = newTime;
            LH_CORE_INFO("Successfully reloaded model '{0}'", name);
            return true;
        }
        catch (const std::exception& e) {
            LH_CORE_ERROR("Failed to reload model '{0}': {1}", name, e.what());
            return false;
        }
    }

    void ModelLibrary::ReloadAll()
    {
        std::unique_lock lock(s_Mutex);
        LH_CORE_INFO("Reloading all models...");

        size_t successCount = 0;
        size_t failCount = 0;

        for (auto& [name, record] : s_Models)
        {
            if (record.sourcePath.empty()) continue;

            namespace fs = std::filesystem;
            if (!fs::exists(record.sourcePath))
            {
                LH_CORE_ERROR("Model source missing: {0}", record.sourcePath.string());
                failCount++;
                continue;
            }

            const auto newTime = fs::last_write_time(record.sourcePath);
            if (newTime <= record.lastModified)
                continue;

            try {
                auto newModel = std::make_shared<Model>(record.sourcePath);
                if (!newModel || newModel->GetMeshes().empty())
                    throw std::runtime_error("Empty model");

                record.model = newModel;
                record.lastModified = newTime;
                successCount++;
            }
            catch (const std::exception& e) {
                LH_CORE_ERROR("Reload failed for '{0}': {1}", name, e.what());
                failCount++;
            }
        }

        LH_CORE_INFO("Reloaded models: {0} succeeded, {1} failed", successCount, failCount);
    }
}
