#include "luthpch.h"
#include "luth/resources/libraries/TextureCache.h"
#include "luth/resources/ResourceDB.h"

namespace Luth
{
    std::shared_mutex TextureCache::s_Mutex;
    std::unordered_map<UUID, TextureCache::TextureRecord, UUIDHash> TextureCache::s_Textures;

    void TextureCache::Init()
    {
        std::unique_lock lock(s_Mutex);
        LH_CORE_INFO("Initialized Texture Cache");
    }

    void TextureCache::Shutdown()
    {
        std::unique_lock lock(s_Mutex);
        s_Textures.clear();
        LH_CORE_INFO("Cleared Texture Cache");
    }

    bool TextureCache::Add(std::shared_ptr<Texture> texture)
    {
        if (!texture) {
            LH_CORE_ERROR("Attempted to add null texture");
            return false;
        }

        UUID uuid = texture->GetUUID();
        std::unique_lock lock(s_Mutex);
        auto [it, inserted] = s_Textures.try_emplace(uuid, TextureRecord{ texture, {} });

        if (!inserted) {
            LH_CORE_WARN("Texture with UUID {0} already exists! Overwriting...", uuid.ToString());
            it->second = { texture, {} };
        }
        return true;
    }

    bool TextureCache::Remove(const UUID& uuid)
    {
        std::unique_lock lock(s_Mutex);
        return s_Textures.erase(uuid) > 0;
    }

    bool TextureCache::Contains(const UUID& uuid)
    {
        std::shared_lock lock(s_Mutex);
        return s_Textures.find(uuid) != s_Textures.end();
    }

    std::shared_ptr<Texture> TextureCache::Get(const UUID& uuid)
    {
        std::shared_lock lock(s_Mutex);
        auto it = s_Textures.find(uuid);
        return it != s_Textures.end() ? it->second.Texture : nullptr;
    }

    std::unordered_map<UUID, TextureCache::TextureRecord, UUIDHash> TextureCache::GetAllTextures()
    {
        return s_Textures;
    }

    std::vector<UUID> TextureCache::GetAllUuids()
    {
        std::shared_lock lock(s_Mutex);
        std::vector<UUID> uuids;
        uuids.reserve(s_Textures.size());
        for (const auto& [uuid, _] : s_Textures)
            uuids.push_back(uuid);
        return uuids;
    }

    std::shared_ptr<Texture> TextureCache::Load(const fs::path& path)
    {
        if (!fs::exists(path)) {
            LH_CORE_ERROR("Texture file not found: {0}", path.string());
            return nullptr;
        }

        UUID uuid = ResourceDB::GetUuidForPath(path);
        if (auto existing = Get(uuid)) {
            LH_CORE_INFO("Texture already loaded: {0}", uuid.ToString());
            return existing;
        }

        auto texture = Create(path);
        if (!texture) {
            LH_CORE_ERROR("Failed to load texture from {0}", path.string());
            return nullptr;
        }

        texture->SetUUID(uuid);
        auto modTime = fs::last_write_time(path);

        std::unique_lock lock(s_Mutex);
        s_Textures[uuid] = { texture, modTime };
        LH_CORE_TRACE("Loaded texture as {0}", uuid.ToString());
        return texture;
    }

    std::shared_ptr<Texture> TextureCache::LoadOrGet(const fs::path& path)
    {
        UUID uuid = ResourceDB::GetUuidForPath(path);
        if (auto texture = Get(uuid)) {
            return texture;
        }
        return Load(path);
    }

    std::shared_ptr<Texture> TextureCache::Create(const fs::path& path)
    {
        return Texture::Create(path);
    }

    std::shared_ptr<Texture> TextureCache::Create(uint32_t width, uint32_t height, TextureFormat format)
    {
        return Texture::Create(width, height, format);
    }

    bool TextureCache::Reload(const UUID& uuid)
    {
        std::unique_lock lock(s_Mutex);
        auto it = s_Textures.find(uuid);
        if (it == s_Textures.end()) {
            LH_CORE_WARN("Cannot reload non-existent texture {0}", uuid.ToString());
            return false;
        }

        auto path = ResourceDB::ResolveUuid(uuid);
        if (path.empty()) {
            LH_CORE_ERROR("No source path for texture {0}", uuid.ToString());
            return false;
        }

        if (!fs::exists(path)) {
            LH_CORE_ERROR("Texture source file missing: {0}", path.string());
            return false;
        }

        const auto newTime = fs::last_write_time(path);
        if (newTime <= it->second.LastModified)
            return true; // Already up-to-date

        try {
            auto newTexture = Create(path);
            if (!newTexture) {
                throw std::runtime_error("Texture creation failed");
            }

            newTexture->SetUUID(uuid);
            it->second = { newTexture, newTime };
            LH_CORE_INFO("Successfully reloaded texture {0}", uuid.ToString());
            return true;
        }
        catch (const std::exception& e) {
            LH_CORE_ERROR("Failed to reload texture {0}: {1}", uuid.ToString(), e.what());
            return false;
        }
    }

    void TextureCache::ReloadAll()
    {
        std::unique_lock lock(s_Mutex);
        LH_CORE_INFO("Reloading all textures...");

        size_t successCount = 0;
        size_t failCount = 0;

        for (auto& [uuid, record] : s_Textures) {
            auto path = ResourceDB::ResolveUuid(uuid);
            if (path.empty()) {
                LH_CORE_WARN("Skipping texture {0} with invalid path", uuid.ToString());
                failCount++;
                continue;
            }

            if (!fs::exists(path)) {
                LH_CORE_ERROR("Texture source missing: {0}", path.string());
                failCount++;
                continue;
            }

            const auto newTime = fs::last_write_time(path);
            if (newTime <= record.LastModified)
                continue;

            try {
                auto newTexture = Create(path);
                if (!newTexture) {
                    throw std::runtime_error("Texture creation failed");
                }

                newTexture->SetUUID(uuid);
                record = { newTexture, newTime };
                successCount++;
            }
            catch (const std::exception& e) {
                LH_CORE_ERROR("Reload failed for {0}: {1}", uuid.ToString(), e.what());
                failCount++;
            }
        }

        LH_CORE_INFO("Reloaded textures: {0} succeeded, {1} failed", successCount, failCount);
    }
}
