#pragma once

#include <filesystem>
#include <string>
#include <array>

namespace fs = std::filesystem;

namespace Luth
{
    class ProjectTextureIndex;

    // Common sibling directory names DCCs drop textures into, relative to the model. Shared by the
    // resolver and AssetDatabase::IngestFile so discovery and binding agree on where to look.
    inline constexpr std::array<const char*, 10> k_CommonTextureDirs = {
        "textures", "Textures", "texture", "Texture", "tex", "Tex", "maps", "Maps", "images", "Images"
    };

    struct ResolveResult
    {
        fs::path ResolvedPath;   // Empty if not found
        std::string Strategy;    // direct, filename_in_parent, sibling_dir, recursive, ext_swap,
                                 // case_insensitive, index_filename, index_stem, fuzzy
    };

    // Locate a texture referenced by Assimp, widening from exact-local to project-wide fuzzy:
    //   1 direct           modelDir / assimpPath (exact relative path from the DCC)
    //   2 filename_in_parent modelDir / filename (strips absolute DCC paths to their basename)
    //   3 sibling_dir      modelDir / <common>/ filename
    //   4 recursive        any file under modelDir with matching filename (depth <= 3)
    //   5 ext_swap         same stem, any image extension, in modelDir + common dirs (.tga vs .png)
    //   6 case_insensitive filename match ignoring case, under modelDir (depth <= 3)
    //   7 index_filename   project-wide exact filename (reaches parent/sibling/nested), nearest wins
    //   8 index_stem       project-wide stem, extension-agnostic
    //   9 fuzzy            project-wide stem-similarity (renamed exports); caller logs the pairing
    // Strategies 7-9 require the ProjectTextureIndex; pass nullptr for the filesystem-only path.
    ResolveResult ResolveTexturePath(const fs::path& modelDir, const std::string& assimpPath,
                                     const ProjectTextureIndex* index = nullptr);
}
