#pragma once

#include "luth/resources/AssetImporter.h"
#include "luth/core/LuthTypes.h"
#include <vector>

namespace Luth
{
    struct ShaderAssetData : public AssetData
    {
        std::vector<u32> VertexSpirV;
        std::vector<u32> FragmentSpirV;
        std::string SourcePath; // Original source path for debug/reload
    };

    class ShaderImporter : public AssetImporter
    {
    public:
        bool Import(const std::filesystem::path& source, const std::filesystem::path& destination) override;
    };
}
