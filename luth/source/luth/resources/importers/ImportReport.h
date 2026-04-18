#pragma once

#include "luth/renderer/material/Material.h"
#include "luth/core/UUID.h"

#include <filesystem>
#include <string>
#include <vector>

namespace Luth
{
    struct UnresolvedTexture
    {
        std::string MaterialName;
        fs::path    MaterialPath;  // Path to the .mat file on disk
        std::string OriginalPath;  // Raw path Assimp reported
        MapType     Type = MapType::Diffuse;

        // Filled in by the user via the remap dialog
        fs::path    UserProvidedPath;
    };

    struct ImportReport
    {
        fs::path ModelPath;
        std::vector<UnresolvedTexture> Unresolved;

        bool HasUnresolved() const { return !Unresolved.empty(); }
        void Clear() { ModelPath.clear(); Unresolved.clear(); }
    };
}
