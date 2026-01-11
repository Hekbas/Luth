#pragma once

#include "luth/core/LuthTypes.h"
#include "luth/resources/Asset.h"
#include "luth/renderer/Buffer.h"
#include "luth/resources/importers/MaterialImporter.h"

#include <vector>
#include <filesystem>

namespace Luth
{
    // Header for all binary assets
    struct AssetHeader
    {
        char Magic[4] = { 'L', 'U', 'T', 'H' };
        u32 Version = 1;
        AssetType Type = AssetType::None;
    };

    // Specific headers
    struct TextureHeader
    {
        u32 Width;
        u32 Height;
        u32 Format; // TextureFormat enum
        u32 SizeBytes;
    };

    struct MeshHeader
    {
        u32 VertexCount;
        u32 IndexCount;
        u32 MaterialIndex;
        // Followed by:
        // - Vertex Data (VertexCount * sizeof(Vertex))
        // - Index Data (IndexCount * sizeof(u32))
    };

    struct ModelHeader
    {
        u32 MeshCount;
        u32 MaterialCount;
        // Followed by:
        // - MeshHeader + Data [MeshCount]
        // - Material UUIDs [MaterialCount]
    };

    class AssetSerializer
    {
    public:
        static bool SerializeTexture(const fs::path& path, const struct TextureAssetData& data);
        static bool DeserializeTexture(const fs::path& path, struct TextureAssetData& outData);

        static bool SerializeModel(const fs::path& path, const struct ModelAssetData& data);
        static bool DeserializeModel(const fs::path& path, struct ModelAssetData& outData);

        static bool SerializeMaterial(const fs::path& path, const struct MaterialAssetData& data);
        static bool DeserializeMaterial(const fs::path& path, struct MaterialAssetData& outData);
    };
}
