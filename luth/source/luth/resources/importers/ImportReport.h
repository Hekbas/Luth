#pragma once

#include "luth/renderer/material/Material.h"
#include "luth/core/UUID.h"

#include <filesystem>
#include <string>
#include <vector>

namespace Luth
{
    // Side-channel result from ModelImporter for textures Assimp couldn't resolve. The editor's
    // TextureRemapDialog reads ImportReport, lets the user point at the missing files, then
    // re-runs the import with the resolved paths bound to UserProvidedPath.
    struct UnresolvedTexture
    {
        std::string MaterialName;
        fs::path    MaterialPath;  // Path to the .mat file on disk
        std::string OriginalPath;  // Raw path Assimp reported
        MapType     Type = MapType::Diffuse;

        // Filled in by the user via the remap dialog
        fs::path    UserProvidedPath;
    };

    // A texture layout the importer routed to a reduced-fidelity fallback (e.g. a separate metal+rough
    // bake that failed). Non-fatal: surfaced so the user knows fidelity dropped and can supply a fix.
    struct DegradedTexture
    {
        std::string MaterialName;
        fs::path    MaterialPath;
        std::string Reason;
    };

    struct ImportReport
    {
        fs::path ModelPath;
        std::vector<UnresolvedTexture> Unresolved;
        std::vector<DegradedTexture> Degraded;

        bool HasUnresolved() const { return !Unresolved.empty(); }
        bool HasDegraded()   const { return !Degraded.empty(); }
        void Clear() { ModelPath.clear(); Unresolved.clear(); Degraded.clear(); }
    };
}
