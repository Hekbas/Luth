#include "luthpch.h"
#include "luth/resources/importers/TextureResolver.h"
#include "luth/resources/importers/ProjectTextureIndex.h"

#include <algorithm>
#include <cctype>

namespace Luth
{
    static constexpr int k_MaxRecursionDepth = 3;

    static const char* k_ImageExts[] = {
        ".png", ".jpg", ".jpeg", ".tga", ".bmp", ".tif", ".tiff", ".dds", ".hdr", ".exr", ".psd"
    };

    static std::string LowerAscii(std::string s)
    {
        std::transform(s.begin(), s.end(), s.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return s;
    }

    // Counts how many directory components a path has (ignores the filename)
    static int DirectoryDepth(const fs::path& path)
    {
        int depth = 0;
        for (auto it = path.begin(); it != path.end(); ++it)
            ++depth;
        return depth - 1; // subtract the filename component
    }

    ResolveResult ResolveTexturePath(const fs::path& modelDir, const std::string& assimpPath,
                                     const ProjectTextureIndex* index)
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
        for (const char* dir : k_CommonTextureDirs)
        {
            fs::path candidate = modelDir / dir / filename;
            if (fs::exists(candidate))
                return { candidate, "sibling_dir" };
        }

        // Strategy 4: recursive search within modelDir (depth-limited), exact filename
        if (fs::exists(modelDir))
        {
            try
            {
                for (const auto& entry : fs::recursive_directory_iterator(
                    modelDir, fs::directory_options::skip_permission_denied))
                {
                    if (!entry.is_regular_file()) continue;
                    fs::path rel = fs::relative(entry.path(), modelDir);
                    if (DirectoryDepth(rel) > k_MaxRecursionDepth) continue;
                    if (entry.path().filename() == filename)
                        return { entry.path(), "recursive" };
                }
            }
            catch (...) {}
        }

        // Strategy 5: extension-swap in the local dirs (DCC referenced .tga, a .png is on disk)
        {
            const std::string stem = assimp.stem().string();
            for (const char* ext : k_ImageExts)
            {
                const fs::path fn = stem + ext;
                if (fs::exists(modelDir / fn))
                    return { modelDir / fn, "ext_swap" };
                for (const char* dir : k_CommonTextureDirs)
                {
                    fs::path candidate = modelDir / dir / fn;
                    if (fs::exists(candidate))
                        return { candidate, "ext_swap" };
                }
            }
        }

        // Strategy 6: case-insensitive filename anywhere under modelDir (depth-limited)
        if (fs::exists(modelDir))
        {
            const std::string wantLower = LowerAscii(filename);
            try
            {
                for (const auto& entry : fs::recursive_directory_iterator(
                    modelDir, fs::directory_options::skip_permission_denied))
                {
                    if (!entry.is_regular_file()) continue;
                    fs::path rel = fs::relative(entry.path(), modelDir);
                    if (DirectoryDepth(rel) > k_MaxRecursionDepth) continue;
                    if (LowerAscii(entry.path().filename().string()) == wantLower)
                        return { entry.path(), "case_insensitive" };
                }
            }
            catch (...) {}
        }

        // Strategies 7-9 reach outside modelDir (parent/sibling/nested) via the project-wide index.
        if (index && !index->Empty())
        {
            if (fs::path p = index->FindByFilename(filename, modelDir); !p.empty())
                return { p, "index_filename" };
            if (fs::path p = index->FindByStem(assimp.stem().string(), modelDir); !p.empty())
                return { p, "index_stem" };
            std::string matched;
            if (fs::path p = index->FindFuzzy(assimp.stem().string(), modelDir, matched); !p.empty())
                return { p, "fuzzy" };
        }

        return {};
    }
}
