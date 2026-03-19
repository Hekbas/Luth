#include "luthpch.h"
#include "ShaderImporter.h"
#include "luth/renderer/ShaderCompiler.h"
#include "luth/resources/AssetSerializer.h"

namespace Luth
{
    bool ShaderImporter::Import(const std::filesystem::path& source, const std::filesystem::path& destination)
    {
        // Determine vertex and fragment paths from source
        // Convention: .vert is the primary asset, matching .frag is found automatically
        fs::path vertPath = source;
        fs::path fragPath = source;

        if (vertPath.extension() == ".vert")
        {
            fragPath.replace_extension(".frag");
        }
        else if (vertPath.extension() == ".frag")
        {
            // If given a .frag, find the matching .vert
            vertPath.replace_extension(".vert");
        }
        else
        {
            LH_CORE_ERROR("ShaderImporter: Unsupported shader extension '{0}'", source.string());
            return false;
        }

        // Compile vertex shader
        if (!fs::exists(vertPath))
        {
            LH_CORE_ERROR("ShaderImporter: Vertex shader not found: {0}", vertPath.string());
            return false;
        }

        auto vertSpv = ShaderCompiler::Compile(vertPath);
        if (vertSpv.empty())
        {
            LH_CORE_ERROR("ShaderImporter: Failed to compile vertex shader: {0}", vertPath.string());
            return false;
        }

        // Compile fragment shader
        if (!fs::exists(fragPath))
        {
            LH_CORE_ERROR("ShaderImporter: Fragment shader not found: {0}", fragPath.string());
            return false;
        }

        auto fragSpv = ShaderCompiler::Compile(fragPath);
        if (fragSpv.empty())
        {
            LH_CORE_ERROR("ShaderImporter: Failed to compile fragment shader: {0}", fragPath.string());
            return false;
        }

        // Pack into asset data
        ShaderAssetData data;
        data.VertexSpirV = std::move(vertSpv);
        data.FragmentSpirV = std::move(fragSpv);
        data.SourcePath = source.string();

        return AssetSerializer::SerializeShader(destination, data);
    }
}
