#pragma once

#include "luth/core/UUID.h"

#include <filesystem>
#include <optional>

namespace Luth
{
    class SceneViewer
    {
    public:
        void Draw(const UUID& sceneUUID, const std::filesystem::path& scenePath);

    private:
        // Cache parsed scene metadata so Draw doesn't re-read + re-parse the
        // file every frame. Invalidated when (uuid, mtime) changes.
        struct CacheEntry {
            UUID                              uuid;
            std::filesystem::file_time_type   mtime;
            int                               entityCount = 0;
            std::uintmax_t                    fileSize    = 0;
        };
        std::optional<CacheEntry> m_Cache;
    };
}
