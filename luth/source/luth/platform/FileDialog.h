#pragma once

#include <filesystem>
#include <optional>

namespace Luth
{
    // Synchronous OS open / save dialog. Blocks the calling thread until the user picks a file,
    // so it must only be called from the main thread between frames (editor menus, project launcher).
    class FileDialog
    {
    public:
        static std::optional<std::filesystem::path> OpenFile(const char* filter);
        static std::optional<std::filesystem::path> SaveFile(const char* filter);
    };
}
