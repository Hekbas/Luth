#pragma once

#include <filesystem>
#include <string>

namespace Luth
{
    /// Represents a .luthproj file — the entry point for a Luth project.
    /// The project root is inferred from the directory containing the file.
    struct ProjectFile
    {
        std::string Name    = "Untitled";
        std::string Version = "0.1";

        // Derived at load time — not serialized
        std::filesystem::path FilePath;      // Absolute path to the .luthproj file
        std::filesystem::path ProjectRoot;   // Directory containing the .luthproj
        std::filesystem::path AssetsRoot;    // ProjectRoot / "assets"

        /// Load a .luthproj file. Returns true on success.
        bool Load(const std::filesystem::path& path);

        /// Save (create or overwrite) a .luthproj file at the given path.
        bool Save(const std::filesystem::path& path) const;

        /// Search for a .luthproj file:
        ///  1. If `hint` is a file path ending in .luthproj, try to load it
        ///  2. If `hint` is a directory, look for *.luthproj in it
        ///  3. If `hint` is empty, search the current working directory
        /// Returns true if a project was found and loaded.
        bool Discover(const std::filesystem::path& hint = "");

        bool IsValid() const { return !ProjectRoot.empty(); }
    };
}
