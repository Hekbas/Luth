#pragma once

#include "luth/resources/AssetImporter.h"
#include <nlohmann/json.hpp>

namespace Luth
{
    // The raw JSON survives the import step intact and is parsed by Material::Deserialize on
    // the main thread, where the live Material can resolve UUID references against AssetManager.
    struct MaterialAssetData : public AssetData
    {
        nlohmann::json JsonData;
    };

    // Imports .lhmat material assets. The .lhmat is itself JSON, so the importer is essentially
    // a copy + validate pass that pins the JSON for later main-thread deserialization.
    class MaterialImporter : public AssetImporter
    {
    public:
        bool Import(const std::filesystem::path& source, const std::filesystem::path& destination) override;
    };
}