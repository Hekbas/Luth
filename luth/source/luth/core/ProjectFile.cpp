#include "luthpch.h"
#include "luth/core/ProjectFile.h"

#include <nlohmann/json.hpp>
#include <fstream>

namespace Luth
{
    namespace fs = std::filesystem;

    bool ProjectFile::Load(const fs::path& path)
    {
        if (!fs::exists(path))
        {
            LH_CORE_ERROR("ProjectFile: File not found: {}", path.string());
            return false;
        }

        try
        {
            std::ifstream file(path);
            nlohmann::json j = nlohmann::json::parse(file);

            Name    = j.value("name", Name);
            Version = j.value("version", Version);

            FilePath    = fs::absolute(path);
            ProjectRoot = FilePath.parent_path();
            AssetsRoot  = ProjectRoot / "assets";

            LH_CORE_INFO("ProjectFile: Loaded '{}' from '{}'", Name, FilePath.string());
            return true;
        }
        catch (const std::exception& e)
        {
            LH_CORE_ERROR("ProjectFile: Failed to parse '{}': {}", path.string(), e.what());
            return false;
        }
    }

    bool ProjectFile::Save(const fs::path& path) const
    {
        try
        {
            nlohmann::json j;
            j["name"]    = Name;
            j["version"] = Version;

            fs::create_directories(path.parent_path());
            std::ofstream file(path);
            file << j.dump(4);

            LH_CORE_INFO("ProjectFile: Saved '{}' to '{}'", Name, path.string());
            return true;
        }
        catch (const std::exception& e)
        {
            LH_CORE_ERROR("ProjectFile: Failed to save '{}': {}", path.string(), e.what());
            return false;
        }
    }

    bool ProjectFile::Discover(const fs::path& hint)
    {
        // Direct file path
        if (!hint.empty() && hint.extension() == ".luthproj")
            return Load(hint);

        // Search a directory
        fs::path searchDir = hint.empty() ? fs::current_path() : hint;
        if (!fs::is_directory(searchDir))
            searchDir = searchDir.parent_path();

        if (!fs::exists(searchDir))
            return false;

        for (const auto& entry : fs::directory_iterator(searchDir))
        {
            if (entry.is_regular_file() && entry.path().extension() == ".luthproj")
                return Load(entry.path());
        }

        LH_CORE_WARN("ProjectFile: No .luthproj found in '{}'", searchDir.string());
        return false;
    }
}
