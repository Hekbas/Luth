#include "luthpch.h"
#include "luth/resources/libraries/ShaderLibrary.h"
#include "luth/resources/ResourceDB.h"

namespace Luth
{
    std::shared_mutex ShaderLibrary::s_Mutex;
    std::unordered_map<UUID, ShaderLibrary::ShaderRecord, UUIDHash> ShaderLibrary::s_Shaders;

    void ShaderLibrary::Init()
    {
        std::unique_lock lock(s_Mutex);
        LH_CORE_INFO("Initialized Shader Library");
    }

    void ShaderLibrary::Shutdown()
    {
        std::unique_lock lock(s_Mutex);
        s_Shaders.clear();
        LH_CORE_INFO("Cleared Shader Library");
    }

    bool ShaderLibrary::Add(std::shared_ptr<Shader> shader)
    {
        if (!shader) {
            LH_CORE_ERROR("Attempted to add null Shader");
            return false;
        }

        UUID uuid = shader->GetUUID();
        std::unique_lock lock(s_Mutex);

        auto [it, inserted] = s_Shaders.try_emplace(uuid,
            ShaderRecord{ shader, "", fs::file_time_type() }
        );

        if (!inserted) {
            LH_CORE_WARN("Shader with UUID {0} already exists! Overwriting...", uuid.ToString());
            it->second = { shader, "", fs::file_time_type() };
        }
        return true;
    }

    bool ShaderLibrary::Remove(const UUID& uuid)
    {
        std::unique_lock lock(s_Mutex);
        return s_Shaders.erase(uuid) > 0;
    }

    bool ShaderLibrary::Contains(const UUID& uuid)
    {
        std::shared_lock lock(s_Mutex);
        return s_Shaders.find(uuid) != s_Shaders.end();
    }

    std::shared_ptr<Shader> ShaderLibrary::Get(const UUID& uuid)
    {
        std::shared_lock lock(s_Mutex);
        auto it = s_Shaders.find(uuid);
        return it != s_Shaders.end() ? it->second.Shader : nullptr;
    }

    std::vector<UUID> ShaderLibrary::GetAllUuids()
    {
        std::shared_lock lock(s_Mutex);
        std::vector<UUID> uuids;
        uuids.reserve(s_Shaders.size());
        for (const auto& [uuid, _] : s_Shaders)
            uuids.push_back(uuid);
        return uuids;
    }

    std::unordered_map<UUID, ShaderLibrary::ShaderRecord, UUIDHash> ShaderLibrary::GetAllShaders()
    {
        return s_Shaders;
    }

    std::shared_ptr<Shader> ShaderLibrary::Load(const fs::path& filePath)
    {
        if (!fs::exists(filePath)) {
            LH_CORE_ERROR("Shader file not found: {0}", filePath.string());
            return nullptr;
        }

        UUID uuid = ResourceDB::GetUuidForPath(filePath);
        if (auto existing = Get(uuid)) {
            LH_CORE_INFO("Shader already loaded: {0}", uuid.ToString());
            return existing;
        }

        auto shader = Shader::Create(filePath);
        if (!shader) {
            LH_CORE_ERROR("Failed to compile shader from {0}", filePath.string());
            return nullptr;
        }

        auto modTime = fs::last_write_time(filePath);

        std::unique_lock lock(s_Mutex);
        s_Shaders[uuid] = { shader, filePath, modTime };
        shader->SetUUID(uuid);

        LH_CORE_INFO("Loaded Shader {0} from {1}", uuid.ToString(), filePath.string());
        return shader;
    }

    std::shared_ptr<Shader> ShaderLibrary::LoadOrGet(const fs::path& filePath)
    {
        UUID uuid = ResourceDB::GetUuidForPath(filePath);
        if (auto shader = Get(uuid)) {
            return shader;
        }
        return Load(filePath);
    }

    bool ShaderLibrary::Reload(const UUID& uuid)
    {
        std::unique_lock lock(s_Mutex);
        auto it = s_Shaders.find(uuid);
        if (it == s_Shaders.end()) {
            LH_CORE_WARN("Cannot reload non-existent Shader {0}", uuid.ToString());
            return false;
        }

        auto& record = it->second;
        if (record.SourcePath.empty()) {
            LH_CORE_INFO("Shader {0} not reloadable (no source path)", uuid.ToString());
            return false;
        }

        if (!fs::exists(record.SourcePath)) {
            LH_CORE_ERROR("Shader source missing: {0}", record.SourcePath.string());
            return false;
        }

        const auto newTime = fs::last_write_time(record.SourcePath);
        if (newTime <= record.LastModified)
            return true; // Already up-to-date

        try {
            auto newShader = Shader::Create(record.SourcePath);
            if (!newShader)
                throw std::runtime_error("Compilation failed");

            newShader->SetUUID(uuid);
            record.Shader = newShader;
            record.LastModified = newTime;

            LH_CORE_INFO("Successfully reloaded Shader {0}", uuid.ToString());
            return true;
        }
        catch (const std::exception& e) {
            LH_CORE_ERROR("Failed to reload Shader {0}: {1}", uuid.ToString(), e.what());
            return false;
        }
    }

    void ShaderLibrary::ReloadAll()
    {
        std::unique_lock lock(s_Mutex);
        LH_CORE_INFO("Reloading all shaders...");

        size_t successCount = 0;
        size_t failCount = 0;

        for (auto& [uuid, record] : s_Shaders) {
            if (record.SourcePath.empty()) continue;

            if (!fs::exists(record.SourcePath)) {
                LH_CORE_ERROR("Shader source missing: {0}", record.SourcePath.string());
                failCount++;
                continue;
            }

            const auto newTime = fs::last_write_time(record.SourcePath);
            if (newTime <= record.LastModified)
                continue;

            try {
                auto newShader = Shader::Create(record.SourcePath);
                if (!newShader)
                    throw std::runtime_error("Compilation failed");

                newShader->SetUUID(uuid);
                record.Shader = newShader;
                record.LastModified = newTime;
                successCount++;
            }
            catch (const std::exception& e) {
                LH_CORE_ERROR("Reload failed for {0}: {1}", uuid.ToString(), e.what());
                failCount++;
            }
        }

        LH_CORE_INFO("Reloaded Shaders: {0} succeeded, {1} failed", successCount, failCount);
    }
}
