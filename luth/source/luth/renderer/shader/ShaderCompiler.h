#pragma once

#include "luth/core/types/LuthTypes.h"
#include "luth/renderer/shader/Shader.h"
#include <filesystem>
#include <vector>

namespace Luth
{
    class ShaderCompiler
    {
    public:
        // Infer shader stage from file extension (.vert / .frag / .comp).
        // Returns ShaderStage::Unknown if the extension is not a known shader stage.
        static ShaderStage InferStage(const std::filesystem::path& path);

        // Compile GLSL source to SPIR-V. Stage is inferred from extension.
        // Returns empty vector on failure.
        static std::vector<u32> Compile(const std::filesystem::path& sourcePath, bool optimize = false);
    };
}
