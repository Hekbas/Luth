#include "luthpch.h"
#include "luth/resources/MaterialLibrary.h"
#include "luth/resources/ShaderLibrary.h"
#include "luth/resources/MaterialLibrary.h"

namespace Luth
{
    void MaterialLibrary::Init()
    {
        std::unique_lock lock(s_Mutex);
        LH_CORE_INFO("Initialized Material Library");
    }

    void MaterialLibrary::Shutdown()
    {
        std::unique_lock lock(s_Mutex);
        s_Materials.clear();
        LH_CORE_INFO("Cleared Material Library");
    }

    bool MaterialLibrary::Add(const std::string& name, std::shared_ptr<Material> material)
    {
        std::unique_lock lock(s_Mutex);
        if (!material || !material->GetShader())
        {
            LH_CORE_ERROR("Attempted to add invalid material '{0}'", name);
            return false;
        }

        auto [it, inserted] = s_Materials.try_emplace(name, MaterialRecord{ material, "", {}, {} });
        if (!inserted)
        {
            LH_CORE_WARN("Material '{0}' already exists! Overwriting...", name);
            it->second = { material, "", {}, {} };
        }
        return true;
    }

    bool MaterialLibrary::Remove(const std::string& name)
    {
        std::unique_lock lock(s_Mutex);
        return s_Materials.erase(name) > 0;
    }

    std::shared_ptr<Material> MaterialLibrary::Get(const std::string& name)
    {
        std::shared_lock lock(s_Mutex);
        auto it = s_Materials.find(name);
        if (it == s_Materials.end())
        {
            LH_CORE_ERROR("Material '{0}' not found in library", name);
            return nullptr;
        }
        return it->second.material;
    }

    bool MaterialLibrary::Contains(const std::string& name)
    {
        std::shared_lock lock(s_Mutex);
        return s_Materials.find(name) != s_Materials.end();
    }

    std::vector<std::string> MaterialLibrary::GetMaterialNames()
    {
        std::shared_lock lock(s_Mutex);
        std::vector<std::string> names;
        names.reserve(s_Materials.size());
        for (const auto& [name, _] : s_Materials)
            names.push_back(name);
        return names;
    }

    std::shared_ptr<Material> MaterialLibrary::Load(const std::filesystem::path& path)
    {
        // TODO: Add material deserialization logic here
        /*auto material = CreateMaterialFromFile(path);
        if (!material || !material->GetShader())
        {
            LH_CORE_ERROR("Failed to create valid material from {0}", path.string());
            return nullptr;
        }

        const std::string name = path.stem().string();
        const auto modTime = std::filesystem::last_write_time(path);

        std::unique_lock lock(s_Mutex);
        s_Materials[name] = { material, path, modTime, CollectDependencies(material) };
        LH_CORE_INFO("Loaded material '{0}' from {1}", name, path.string());
        return material;*/
        return nullptr;
    }

    std::shared_ptr<Material> MaterialLibrary::Load(const std::string& name, const std::filesystem::path& path)
    {
        /*auto material = CreateMaterialFromFile(path);
        if (!material || !material->GetShader())
        {
            LH_CORE_ERROR("Failed to create material '{0}' from {1}", name, path.string());
            return nullptr;
        }

        const auto modTime = std::filesystem::last_write_time(path);

        std::unique_lock lock(s_Mutex);
        s_Materials[name] = { material, path, modTime, CollectDependencies(material) };
        LH_CORE_INFO("Loaded material '{0}' from {1}", name, path.string());
        return material;*/
        return nullptr;
    }

    std::shared_ptr<Material> MaterialLibrary::LoadOrGet(const std::filesystem::path& path)
    {
        const std::string name = path.stem().string();
        std::shared_lock lock(s_Mutex);
        if (auto it = s_Materials.find(name); it != s_Materials.end())
            return it->second.material;
        lock.unlock();
        return Load(path);
    }

    std::shared_ptr<Material> MaterialLibrary::LoadOrGet(const std::string& name, const std::filesystem::path& path)
    {
        std::shared_lock lock(s_Mutex);
        if (auto it = s_Materials.find(name); it != s_Materials.end())
            return it->second.material;
        lock.unlock();
        return Load(name, path);
    }

    bool MaterialLibrary::Reload(const std::string& name)
    {
        std::unique_lock lock(s_Mutex);
        auto it = s_Materials.find(name);
        if (it == s_Materials.end())
        {
            LH_CORE_WARN("Cannot reload non-existent material '{0}'", name);
            return false;
        }

        auto& record = it->second;
        if (record.sourcePath.empty())
        {
            LH_CORE_INFO("Material '{0}' not reloadable (no source path)", name);
            return false;
        }

        namespace fs = std::filesystem;
        if (!fs::exists(record.sourcePath))
        {
            LH_CORE_ERROR("Material source missing: {0}", record.sourcePath.string());
            return false;
        }

        const auto newTime = fs::last_write_time(record.sourcePath);
        if (newTime <= record.lastModified)
            return true; // Already current

        //try {
        //    auto newMaterial = CreateMaterialFromFile(record.sourcePath);
        //    if (!newMaterial || !newMaterial->GetShader())
        //        throw std::runtime_error("Invalid material data");

        //    // Preserve runtime modifications if any
        //    record.material = newMaterial;
        //    record.lastModified = newTime;
        //    record.dependencies = CollectDependencies(newMaterial);

        //    LH_CORE_INFO("Reloaded material '{0}'", name);
        //    return true;
        //}
        //catch (const std::exception& e) {
        //    LH_CORE_ERROR("Failed to reload material '{0}': {1}", name, e.what());
        //    return false;
        //}
        return true;
    }

    void MaterialLibrary::ReloadAll()
    {
        std::unique_lock lock(s_Mutex);
        LH_CORE_INFO("Reloading materials...");

        size_t success = 0;
        size_t failed = 0;

        for (auto& [name, record] : s_Materials)
        {
            if (record.sourcePath.empty()) continue;

            try {
                if (Reload(name)) success++;
                else failed++;
            }
            catch (...) {
                failed++;
            }
        }

        LH_CORE_INFO("Materials reloaded: {0} succeeded, {1} failed", success, failed);
    }

    std::unordered_set<std::string> CollectDependencies(const std::shared_ptr<Material>& material)
    {
        std::unordered_set<std::string> deps;

        // Track shader
        /*if (auto shader = material->GetShader())
        {
            if (auto shaderName = ShaderLibrary::GetName(shader))
                deps.insert(*shaderName);
        }*/

        // Track textures
        /*for (const auto& texInfo : material->GetTextures())
        {
            if (texInfo.texture && !texInfo.path.empty())
            {
                deps.insert(texInfo.path);
            }
        }*/

        return deps;
    }
}
