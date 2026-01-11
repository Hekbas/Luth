#pragma once

#include "luth/resources/AssetImporter.h"
#include <nlohmann/json.hpp>

namespace Luth
{
    struct MaterialAssetData : public AssetData
    {
        // We store the raw JSON to deserialize on the main thread, 
        // or we could parse into a struct here. 
        // Since Material::Deserialize takes JSON, let's store that.
        nlohmann::json JsonData;
    };

    class MaterialImporter : public AssetImporter
    {
    public:
        bool Import(const std::filesystem::path& source, const std::filesystem::path& destination) override;
    };
}