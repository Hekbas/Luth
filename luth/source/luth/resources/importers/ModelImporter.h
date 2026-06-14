#pragma once

#include "luth/resources/AssetImporter.h"
#include "luth/resources/importers/ImportReport.h"
#include "luth/renderer/resources/Model.h"
#include "luth/renderer/resources/Skeleton.h"
#include "luth/renderer/resources/AnimationClip.h"

#include <nlohmann/json.hpp>

namespace Luth
{
    // Imports source mesh files (.fbx, .obj, .gltf) into Library/-resident ModelAssetData artifacts.
    // Splits skinning into its own ModelAssetData::Skeleton and extracts each animation channel into
    // its own .anim sibling asset (when ExtractClipsAsSeparateAssets is on).
    // Decode + processing happens on a worker fiber via JobSystem.
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

        // When true, each animation clip from the source becomes its own .anim sibling asset (UUID-addressable, shareable across rigs).
        // Disable to kip clip extraction entirely.
        bool ExtractClipsAsSeparateAssets = true;

        // Scene-graph extras (static models): import source cameras / lights as entities.
        bool ImportCameras = true;
        bool ImportLights  = true;

        // Stamp an inferred TextureRole into each resolved texture's .meta (Assimp semantic + filename
        // suffix), so packed/non-standard layouts canonicalize at import. Never clobbers a user-set role.
        bool AutoDetectTextureRoles = true;

        // Physics shape sourcing. None (default) → ShapeCache returns null for asset-backed colliders referencing
        // this model and warns once per UUID. Auto → ShapeCache builds JPH::ConvexHullShape / JPH::MeshShape on
        // demand from Model::m_MeshesData. Per-mesh override + actual on-disk shape cooking are deferred to a future effort.
        enum class PhysicsBakeMode : int { None = 0, Auto = 1 };
        PhysicsBakeMode PhysicsBake = PhysicsBakeMode::None;

        static ModelImportSettings FromJson(const nlohmann::json& j);
        nlohmann::json ToJson() const;
    };

    struct ModelAssetData : public AssetData
    {
        std::vector<MeshData> Meshes;
        std::vector<UUID> Materials;
        Skeleton SkeletonData;
        std::vector<UUID> AnimationClipUUIDs;
        bool IsSkinned = false;

        // V4 scene graph — populated for static models only (skinned models stay empty).
        std::vector<ModelNode>   Nodes;
        std::vector<ModelCamera> Cameras;
        std::vector<ModelLight>  Lights;
    };

    class ModelImporter : public AssetImporter
    {
    public:
        bool Import(const std::filesystem::path& source, const std::filesystem::path& destination) override;

        // Returns the report from the most recent Import() call (thread-safe read after import completes)
        static ImportReport GetLastImportReport();
    };
}