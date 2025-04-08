#pragma once

#include "luth/core/UUID.h"
#include "luth/renderer/Texture.h"

#include <memory>
#include <shared_mutex>
#include <unordered_map>

namespace Luth
{
	class TextureCache
	{
	public:
        struct TextureRecord {
            std::shared_ptr<Texture> Texture;
            fs::file_time_type LastModified;
        };

        static void Init();
        static void Shutdown();

        static bool Add(std::shared_ptr<Texture> texture);
        static bool Remove(const UUID& uuid);
        static bool Contains(const UUID& uuid);

        static std::shared_ptr<Texture> Get(const UUID& uuid);
        static std::unordered_map<UUID, TextureRecord, UUIDHash> GetAllTextures();
        static std::vector<UUID> GetAllUuids();

        static std::shared_ptr<Texture> GetDefaultWhite() { return s_White; }
        static std::shared_ptr<Texture> GetDefaultBlack() { return s_Black; }
        static std::shared_ptr<Texture> GetDefaultGrey() { return s_Grey; }
        static std::shared_ptr<Texture> GetDefaultMissing() { return s_Missing; }

        static std::shared_ptr<Texture> Load(const fs::path& path);
        static std::shared_ptr<Texture> LoadOrGet(const fs::path& path);

        static bool Reload(const UUID& uuid);
        static void ReloadAll();

    private:
        static std::shared_ptr<Texture> Create(const fs::path& path);
        static std::shared_ptr<Texture> Create(u32 width, u32 height,
            u32 format, const unsigned char* data);

        static void CreateDefaultTextures();

        static std::shared_mutex s_Mutex;
        static std::unordered_map<UUID, TextureRecord, UUIDHash> s_Textures;

        // Default Textures
        static std::shared_ptr<Texture> s_White;
        static std::shared_ptr<Texture> s_Black;
        static std::shared_ptr<Texture> s_Grey;
        static std::shared_ptr<Texture> s_Missing;
	};
}
