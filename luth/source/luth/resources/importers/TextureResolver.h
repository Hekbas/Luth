#pragma once

#include <filesystem>
#include <string>

namespace fs = std::filesystem;

namespace Luth
{
    struct ResolveResult
    {
        fs::path ResolvedPath;   // Empty if not found
        std::string Strategy;    // "direct", "filename_in_parent", "sibling_dir", "recursive"
    };

    // Tries multiple strategies to locate a texture file referenced by Assimp.
    // Searches in order (stops at first match):
    //   1. Direct:           modelDir / assimpPath  (exact relative path from DCC)
    //   2. Filename-only:    modelDir / filename    (strip any subdir prefix)
    //   3. Sibling dirs:     modelDir / <common>/ filename  (textures, Textures, tex, maps, ...)
    //   4. Recursive:        any file under modelDir with matching filename (depth <= 3)
    ResolveResult ResolveTexturePath(const fs::path& modelDir, const std::string& assimpPath);
}
