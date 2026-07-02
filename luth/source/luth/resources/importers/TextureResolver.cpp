#include "luthpch.h"
#include "luth/resources/importers/TextureResolver.h"

namespace Luth
{
    static constexpr int k_MaxRecursionDepth = 3;

    static const char* s_CommonTextureDirs[] = {
        "textures", "Textures", "texture", "Texture",
        "tex",      "Tex",
        "maps",     "Maps",
        "images",   "Images",
    };

    // Counts how many directory components a path has (ignores the filename)
    static int DirectoryDepth(const fs::path& path)
    {
        int depth = 0;
        for (auto it = path.begin(); it != path.end(); ++it)
            ++depth;
        return depth - 1; // subtract the filename component
    }

    ResolveResult ResolveTexturePath(const fs::path& modelDir, const std::string& assimpPath)
    {
        if (assimpPath.empty())
            return {};

        fs::path assimp(assimpPath);
        std::string filename = assimp.filename().string();
        if (filename.empty())
            return {};

        // Strategy 1: direct path as Assimp reported it
        {
            fs::path candidate = modelDir / assimpPath;
            if (fs::exists(candidate))
                return { candidate, "direct" };
        }

        // Strategy 2: filename only, in model directory
        {
            fs::path candidate = modelDir / filename;
            if (fs::exists(candidate))
                return { candidate, "filename_in_parent" };
        }

        // Strategy 3: common sibling texture directories
        for (const char* dir : s_CommonTextureDirs)
        {
            fs::path candidate = modelDir / dir / filename;
            if (fs::exists(candidate))
                return { candidate, "sibling_dir" };
        }

        // Strategy 4: recursive search within modelDir (depth-limited)
        if (fs::exists(modelDir))
        {
            fs::path modelDirAbs = fs::absolute(modelDir);
            try
            {
                for (const auto& entry : fs::recursive_directory_iterator(
                    modelDir,
                    fs::directory_options::skip_permission_denied))
                {
                    if (!entry.is_regular_file()) continue;

                    // Depth guard: count components relative to modelDir
                    fs::path rel = fs::relative(entry.path(), modelDir);
                    if (DirectoryDepth(rel) > k_MaxRecursionDepth) continue;

                    if (entry.path().filename() == filename)
                        return { entry.path(), "recursive" };
                }
            }
            catch (...) {}
        }

        return {};
    }
}
