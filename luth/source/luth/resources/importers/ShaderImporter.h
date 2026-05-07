#pragma once

#include "luth/resources/AssetImporter.h"
#include "luth/core/types/LuthTypes.h"
#include "luth/renderer/shader/Shader.h" // ShaderStage
#include <vector>

namespace Luth
{
    // Single-stage shader asset payload. One file on disk = one stage = one artifact.
    struct ShaderAssetData : public AssetData
    {
        ShaderStage      Stage = ShaderStage::Unknown;
        std::vector<u32> SpirV;
        std::string      SourcePath; // Original source path for debug/reload
    };

    // Compiles GLSL shader sources to SPIR-V via ShaderCompiler and writes the result into a
    // Library/-resident ShaderAssetData artifact. One source file produces one artifact for one
    // pipeline stage; multi-stage shaders use multiple .vert/.frag/.comp source files.
    class ShaderImporter : public AssetImporter
    {
    public:
        bool Import(const std::filesystem::path& source, const std::filesystem::path& destination) override;
    };
}
