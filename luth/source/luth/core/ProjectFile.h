#pragma once

#include "luth/core/Version.h"

#include <filesystem>
#include <string>

namespace Luth
{
    // The .luthproj file — entry point for a Luth project. ProjectRoot is inferred from the
    // directory containing the file, and AssetsRoot is the conventional ProjectRoot / "assets".
    struct ProjectFile
    {
        std::string Name    = "Untitled";
        std::string Version = Luth::GetVersionString();

        // Derived at load time, not serialized.
        std::filesystem::path FilePath;      // Absolute path to the .luthproj file
        std::filesystem::path ProjectRoot;   // Directory containing the .luthproj
        std::filesystem::path AssetsRoot;    // ProjectRoot / "assets"

        // Load a .luthproj file. Returns true on success.
        bool Load(const std::filesystem::path& path);

        // Save (create or overwrite) a .luthproj file at the given path.
        bool Save(const std::filesystem::path& path) const;

        // Find a .luthproj from a flexible hint:
        //   - file path ending in .luthproj: try to load it directly
        //   - directory: search for *.luthproj inside it
        //   - empty: search the current working directory
        // Returns true if a project was found and loaded.
        bool Discover(const std::filesystem::path& hint = "");

        bool IsValid() const { return !ProjectRoot.empty(); }
    };
}
