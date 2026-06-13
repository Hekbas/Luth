#pragma once

#include "luth/core/types/LuthTypes.h"
#include "luth/renderer/shader/Shader.h"
#include <filesystem>
#include <vector>

namespace Luth
{
    // GLSL-to-SPIR-V compiler shim. Wraps shaderc; AssetManager calls Compile during shader import
    // (pipeline-line) and ShaderWatcher calls it again on hot reload. Compile runs synchronously
    // — the caller dispatches it from a worker fiber, so the glslangValidator backend doesn't
    // block the main thread.
    class ShaderCompiler
    {
    public:
        // Infer shader stage from file extension (.vert / .frag / .comp).
        // Returns ShaderStage::Unknown if the extension is not a known shader stage.
        static ShaderStage InferStage(const std::filesystem::path& path);

        // Compile GLSL source to SPIR-V. Stage is inferred from extension.
        // Returns empty vector on failure.
        static std::vector<u32> Compile(const std::filesystem::path& sourcePath, bool optimize = false);

        // Compile + resolve the pipeline stage together, for the asset importer. GLSL infers the stage
        // from the extension; .slang reads it from the [shader("...")] attribute via reflection (the
        // extension can't carry it). An empty spirv or Unknown stage means "not a single-stage shader
        // asset", so the importer skips it instead of writing a stage-less artifact.
        struct StagedSpirv { std::vector<u32> spirv; ShaderStage stage = ShaderStage::Unknown; };
        static StagedSpirv CompileStaged(const std::filesystem::path& sourcePath, bool optimize = false);
    };
}
