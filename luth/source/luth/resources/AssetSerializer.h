#pragma once

#include "luth/core/types/LuthTypes.h"
#include "luth/resources/Asset.h"
#include "luth/renderer/resources/Buffer.h"
#include "luth/resources/importers/MaterialImporter.h"
#include "luth/resources/importers/ShaderImporter.h"

#include <vector>
#include <filesystem>

namespace Luth
{
    // Binary artifact format that AssetImporter writes to <project>/Library/ and AssetManager
    // reads at load. AssetHeader is the universal magic + version + type prefix; per-type
    // headers (Texture, Mesh, Model, Shader) follow with their specific fields. Format-version
    // bumps are additive: keep the V<n>+ annotations on fields that were added in later versions
    // so deserialize paths know what to expect from older artifacts.
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
        u32 GenerateMipmaps; // bool as u32 for alignment
        u32 WrapMode;        // TextureWrapMode enum
        u32 MinFilter;       // TextureFilterMode enum
        u32 MagFilter;       // TextureFilterMode enum
    };

    struct MeshHeader
    {
        u32 VertexCount;
        u32 IndexCount;
        u32 MaterialIndex;
        u32 IsSkinned;    // bool as u32 for alignment
        // Followed by:
        // - Vertex Data (VertexCount * sizeof(Vertex) or sizeof(SkinnedVertex))
        // - Index Data (IndexCount * sizeof(u32))
    };

    struct ModelHeader
    {
        u32 MeshCount;
        u32 MaterialCount;
        u32 IsSkinned;       // V2+: model-level skinned flag
        u32 BoneCount;       // V2+: number of bones in skeleton
        u32 AnimationCount;  // V2+: number of animation clips
        u32 NodeCount;       // V4+: scene-graph nodes (static models only)
        u32 CameraCount;     // V4+: imported cameras
        u32 LightCount;      // V4+: imported lights
        // Followed by:
        // - Material UUIDs [MaterialCount]
        // - MeshHeader + Data [MeshCount]
        // - (V2+) Skeleton bones [BoneCount]
        // - (V2+) Animation clip UUIDs [AnimationCount]
        // - (V4+) Nodes [NodeCount], Cameras [CameraCount], Lights [LightCount]
    };

    struct ShaderHeader
    {
        u32 Stage;      // ShaderStage enum as u32
        u32 SpirVSize;  // u32 count (not byte count)
        // Followed by:
        // - SpirV [SpirVSize * sizeof(u32)]
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

        static bool SerializeShader(const fs::path& path, const ShaderAssetData& data);
        static bool DeserializeShader(const fs::path& path, ShaderAssetData& outData);

        static bool SerializeAnimation(const fs::path& path, const struct AnimationAssetData& data);
        static bool DeserializeAnimation(const fs::path& path, struct AnimationAssetData& outData);

        static bool SerializePhysicsMaterial  (const fs::path& path, const struct PhysicsMaterialAssetData& data);
        static bool DeserializePhysicsMaterial(const fs::path& path,       struct PhysicsMaterialAssetData& outData);
    };
}
