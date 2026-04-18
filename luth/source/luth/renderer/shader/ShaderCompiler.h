#pragma once

#include "luth/core/LuthTypes.h"
#include <filesystem>
#include <vector>

namespace Luth
{
    class ShaderCompiler
    {
    public:
        // Returns empty vector on failure
        static std::vector<u32> Compile(const std::filesystem::path& sourcePath, bool optimize = false);
    };
}