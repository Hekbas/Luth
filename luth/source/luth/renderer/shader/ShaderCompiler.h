#pragma once

#include "luth/core/types/LuthTypes.h"
#include "luth/renderer/shader/Shader.h"
#include <filesystem>
#include <vector>

namespace Luth
{
    // Slang -> SPIR-V dispatch. The asset importer calls CompileStaged on import; ShaderWatcher + the
    // pipeline subsystems call Compile again on hot reload. Compile runs synchronously on a worker fiber.
    class ShaderCompiler
    {
    public:
        // Compile a .slang shader to SPIR-V. Returns an empty vector on failure / non-.slang input.
        static std::vector<u32> Compile(const std::filesystem::path& sourcePath, bool optimize = false);

        // Compile + resolve the pipeline stage together, for the asset importer. The stage is read from the
        // .slang [shader("...")] attribute via reflection (the extension can't carry it). An empty spirv or
        // Unknown stage means "not a single-stage shader asset", so the importer skips it.
        struct StagedSpirv { std::vector<u32> spirv; ShaderStage stage = ShaderStage::Unknown; };
        static StagedSpirv CompileStaged(const std::filesystem::path& sourcePath, bool optimize = false);
    };
}
