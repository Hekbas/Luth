#pragma once

#include "luth/core/UUID.h"

#include <filesystem>
#include <optional>

namespace Luth
{
    // Read-only inspector pane for Scene assets: shows entity count, file size, and a button to open
    // the scene. Cheaper than the full HierarchyPanel because it doesn't load the scene; it just
    // parses the metadata and caches by (uuid, mtime).
    class SceneViewer
    {
    public:
        void Draw(const UUID& sceneUUID, const std::filesystem::path& scenePath);

    private:
        // Cache parsed scene metadata so Draw doesn't re-read + re-parse the file every frame.
        // Invalidated when (uuid, mtime) changes.
        struct CacheEntry {
            UUID                              uuid;
            std::filesystem::file_time_type   mtime;
            int                               entityCount = 0;
            std::uintmax_t                    fileSize    = 0;
        };
        std::optional<CacheEntry> m_Cache;
    };
}
