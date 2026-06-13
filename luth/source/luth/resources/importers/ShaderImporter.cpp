#include "luthpch.h"
#include "ShaderImporter.h"
#include "luth/renderer/shader/ShaderCompiler.h"
#include "luth/resources/AssetSerializer.h"

namespace Luth
{
    bool ShaderImporter::Import(const std::filesystem::path& source, const std::filesystem::path& destination)
    {
        if (!fs::exists(source))
        {
            LH_CORE_ERROR("ShaderImporter: source not found: {0}", source.string());
            return false;
        }

        // One call covers both languages: GLSL stage from the extension, .slang stage from reflection.
        ShaderCompiler::StagedSpirv compiled = ShaderCompiler::CompileStaged(source);
        if (compiled.spirv.empty() || compiled.stage == ShaderStage::Unknown)
        {
            // A real compile error is already logged by the compiler; a .slang module with no single 'main'
            // entry (e.g. a multi-stage probe) simply isn't a single-stage asset. Nothing to serialize.
            LH_CORE_TRACE("ShaderImporter: no single-stage SPIR-V for '{0}' — skipped", source.string());
            return false;
        }

        ShaderAssetData data;
        data.Stage = compiled.stage;
        data.SpirV = std::move(compiled.spirv);
        data.SourcePath = source.string();

        return AssetSerializer::SerializeShader(destination, data);
    }
}
