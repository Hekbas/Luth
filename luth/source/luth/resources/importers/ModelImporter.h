#pragma once

#include "luth/resources/AssetImporter.h"
#include "luth/renderer/Model.h"
#include "luth/renderer/Skeleton.h"
#include "luth/renderer/AnimationClip.h"

namespace Luth
{
    struct ModelAssetData : public AssetData
    {
        std::vector<MeshData> Meshes;
        std::vector<UUID> Materials;
        Skeleton SkeletonData;
        std::vector<AnimationClip> AnimationClips;
        bool IsSkinned = false;
    };

    class ModelImporter : public AssetImporter
    {
    public:
        bool Import(const std::filesystem::path& source, const std::filesystem::path& destination) override;
    };
}