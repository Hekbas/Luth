#pragma once

#include "luth/resources/AssetImporter.h"
#include "luth/renderer/Model.h" // For MeshData struct

namespace Luth
{
    struct ModelAssetData : public AssetData
    {
        // We reuse the MeshData struct from Model.h for now
        std::vector<MeshData> Meshes;
        std::vector<UUID> Materials;
        // Add skeleton/animation data here later
    };

    class ModelImporter : public AssetImporter
    {
    public:
        bool Import(const std::filesystem::path& source, const std::filesystem::path& destination) override;
    };
}