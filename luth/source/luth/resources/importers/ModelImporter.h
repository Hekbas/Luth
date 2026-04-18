#pragma once

#include "luth/resources/AssetImporter.h"
#include "luth/resources/importers/ImportReport.h"
#include "luth/renderer/resources/Model.h"
#include "luth/animation/Skeleton.h"
#include "luth/animation/AnimationClip.h"

#include <nlohmann/json.hpp>

namespace Luth
{
    struct ModelImportSettings
    {
        // Geometry
        bool ImportNormals    = true;
        bool ImportTangents   = false;
        bool OptimizeMesh     = true;
        float ScaleFactor     = 1.0f;

        // Axis
        int UpAxis            = -1;   // -1 = auto-detect from scene metadata, 0=X, 1=Y, 2=Z
        bool BakeAxisConversion = true;

        // Skinning: how to handle mesh-node transforms relative to skeleton
        enum class MeshTransformMode : int {
            Auto     = 0,  // Detect: apply mesh-node correction if non-identity
            Bake     = 1,  // Always bake mesh-node transform into skeleton
            Identity = 2,  // Ignore mesh-node transform (legacy behavior)
        };
        MeshTransformMode SkinMeshTransform = MeshTransformMode::Auto;

        static ModelImportSettings FromJson(const nlohmann::json& j);
        nlohmann::json ToJson() const;
    };

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

        // Returns the report from the most recent Import() call (thread-safe read after import completes)
        static ImportReport GetLastImportReport();
    };
}