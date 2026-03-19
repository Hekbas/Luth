#pragma once

#include <filesystem>
#include <optional>

namespace Luth
{
    class FileDialog
    {
    public:
        static std::optional<std::filesystem::path> OpenFile(const char* filter);
        static std::optional<std::filesystem::path> SaveFile(const char* filter);
    };
}
