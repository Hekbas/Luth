#pragma once

#include "luth/resources/AssetImporter.h"

#include <nlohmann/json.hpp>

namespace Luth
{
    // Mirrors MaterialAssetData. The .physmat source is JSON; the importer parses + validates,
    // pinning the JSON for main-thread Deserialize into a runtime PhysicsMaterial.
    struct PhysicsMaterialAssetData : public AssetData
    {
        nlohmann::json JsonData;
    };

    // Imports .physmat assets. Same shape as MaterialImporter — small JSON blob (friction /
    // restitution / density), so the importer is a parse + round-trip into the binary artifact.
    class PhysicsMaterialImporter : public AssetImporter
    {
    public:
        bool Import(const std::filesystem::path& source, const std::filesystem::path& destination) override;
    };
}
