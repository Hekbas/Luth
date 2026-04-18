#include "luthpch.h"
#include "ShaderImporter.h"
#include "luth/renderer/shader/ShaderCompiler.h"
#include "luth/resources/AssetSerializer.h"

namespace Luth
{
    bool ShaderImporter::Import(const std::filesystem::path& source, const std::filesystem::path& destination)
    {
        ShaderStage stage = ShaderCompiler::InferStage(source);
        if (stage == ShaderStage::Unknown)
        {
            LH_CORE_ERROR("ShaderImporter: unsupported shader extension for '{0}'", source.string());
            return false;
        }

        if (!fs::exists(source))
        {
            LH_CORE_ERROR("ShaderImporter: source not found: {0}", source.string());
            return false;
        }

        auto spirv = ShaderCompiler::Compile(source);
        if (spirv.empty())
        {
            LH_CORE_ERROR("ShaderImporter: compilation failed for '{0}'", source.string());
            return false;
        }

        ShaderAssetData data;
        data.Stage = stage;
        data.SpirV = std::move(spirv);
        data.SourcePath = source.string();

        return AssetSerializer::SerializeShader(destination, data);
    }
}
