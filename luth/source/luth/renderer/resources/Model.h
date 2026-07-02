#pragma once

#include "luth/core/types/LuthMath.h"
#include "luth/renderer/material/Material.h"
#include "luth/renderer/resources/Mesh.h"
#include "luth/renderer/resources/Skeleton.h"
#include "luth/renderer/resources/AnimationClip.h"
#include "luth/resources/Asset.h"

#include <string>
#include <vector>
#include <memory>
#include <filesystem>

namespace Luth
{
    // Model asset and the in-memory CPU mesh data structs (Vertex, SkinnedVertex, MeshData).
    // ModelImporter (Assimp) reads source files and splits the result into GPU Mesh, Skeleton,
    // and AnimationClip resources. The CPU MeshData lives only long enough to upload through
    // UploadContext; the loaded Model holds GPU handles after that.
    struct Vertex {
        Vec3 Position;
        Vec3 Normal;
        Vec2 TexCoord0;
        Vec2 TexCoord1;
        Vec3 Tangent;
    };

    struct SkinnedVertex {
        Vec3  Position;
        Vec3  Normal;
        Vec2  TexCoord0;
        Vec2  TexCoord1;
        Vec3  Tangent;
        IVec4 BoneIDs    = IVec4(-1);
        Vec4  BoneWeights = Vec4(0.0f);
    };

    struct MeshData {
        std::vector<Vertex> Vertices;
        std::vector<SkinnedVertex> SkinnedVertices;
        std::vector<uint32_t> Indices;
        uint32_t MaterialIndex = 0;
        std::string Name;
        bool IsSkinned = false;
        bool IsDeformable = false;   // static wind-deformable opt-in (per-asset import setting)
        AABB BindPoseAABB;
    };

    struct MeshInfo {
        std::string Name;
        uint32_t VertexCount = 0;
        uint32_t IndexCount = 0;
        uint32_t MaterialIndex = 0;
    };

    struct BoneNodeInfo {
        std::string Name;
        int ParentIndex = -1; // Index in BoneHierarchy vector
        int BoneIndex = -1;   // -1 if not a bone
    };

    struct AnimationInfo {
        std::string Name;
        double Duration = 0.0;
        double TicksPerSecond = 0.0;
    };

    struct ModelInfo {
        fs::path Path;
        bool IsSkinned = false;
        uint32_t TotalMeshCount = 0;
        uint32_t TotalVertexCount = 0;
        uint32_t TotalIndexCount = 0;
        uint32_t MaterialCount = 0;
        std::vector<MeshInfo> Meshes;

        // Skinned model data
        uint32_t BoneCount = 0;
        uint32_t AnimationCount = 0;
        std::vector<BoneNodeInfo> BoneHierarchy;
        std::vector<AnimationInfo> Animations;
    };

    // Scene-graph import (static models only; skinned models reconstruct hierarchy from the
    // skeleton instead). Nodes are topological (parent before child); transforms are LOCAL; the
    // instantiated entity tree composes world transforms. MeshIndices reference the Model's meshes;
    // CameraIndex/LightIndex address the Cameras/Lights arrays (-1 = none).
    struct ModelNode {
        std::string Name;
        i32 ParentIndex = -1;
        Vec3 Translation = Vec3(0.0f);
        Quat Rotation    = Quat(1.0f, 0.0f, 0.0f, 0.0f);
        Vec3 Scale       = Vec3(1.0f);
        std::vector<u32> MeshIndices;
        i32 CameraIndex = -1;
        i32 LightIndex  = -1;
    };

    struct ModelCamera {
        f32 FovYDeg    = 45.0f;    // vertical, degrees
        f32 NearClip   = 0.01f;
        f32 FarClip    = 1000.0f;
        f32 Aspect     = 16.0f / 9.0f;
        i32 Orthographic = 0;      // Assimp is perspective-only; kept for forward-compat
    };

    struct ModelLight {
        i32  Type      = 0;        // 0 = directional, 1 = point, 2 = spot
        Vec3 Color     = Vec3(1.0f);
        f32  Intensity = 1.0f;
        f32  Range     = 350.0f;   // point + spot
        f32  InnerConeAngleDeg = 25.0f;   // spot only (half-angle)
        f32  OuterConeAngleDeg = 45.0f;   // spot only (half-angle)
    };

    class Model : public Asset
    {
    public:
        struct CreateParams;

        virtual AssetType GetType() const override { return AssetType::Model; }
        
        Model() = default;
        virtual ~Model() = default;

        static std::shared_ptr<Model> Create(const std::vector<MeshData>& meshData, const std::vector<UUID>& materials);
        static std::shared_ptr<Model> Create(const std::vector<MeshData>& meshData, const std::vector<UUID>& materials,
            const Skeleton& skeleton, const std::vector<UUID>& clipUUIDs, bool isSkinned);
        
        std::vector<MeshData>& GetMeshesData() { return m_MeshesData; }
        const std::vector<std::shared_ptr<Mesh>>& GetMeshes() const { return m_Meshes; }
        std::shared_ptr<Mesh> GetMesh(u32 index) const { return (index < m_Meshes.size()) ? m_Meshes[index] : nullptr; }
        const ModelInfo& GetCachedModelInfo() const { return m_ModelInfo; }

        void AddMaterial(UUID uuid, u32 index) {
            if (index >= m_Materials.size()) {
                m_Materials.resize(index + 1);
            }
            m_Materials[index] = uuid;
        }
        std::vector<UUID>& GetMaterials() { return m_Materials; }

        bool IsSkinned() const { return m_IsSkinned; }
        void SetIsSkinned(bool value) { m_IsSkinned = value; }

        // Skeleton & Animation. Clips are first-class assets; lookup via AssetManager::GetAsset<AnimationClip>(uuid)
        // using indices into the UUID list.
        const Skeleton& GetSkeleton() const { return m_Skeleton; }
        Skeleton& GetSkeleton() { return m_Skeleton; }
        const std::vector<UUID>& GetAnimationClipUUIDs() const { return m_AnimationClipUUIDs; }
        std::vector<UUID>& GetAnimationClipUUIDs() { return m_AnimationClipUUIDs; }

        // Scene graph (static models). Empty for skinned models; gate reads via HasNodeTree().
        void SetSceneGraph(std::vector<ModelNode> nodes, std::vector<ModelCamera> cameras,
            std::vector<ModelLight> lights) {
            m_Nodes = std::move(nodes); m_Cameras = std::move(cameras); m_Lights = std::move(lights);
        }
        bool HasNodeTree() const { return !m_Nodes.empty(); }
        const std::vector<ModelNode>&   GetNodes()   const { return m_Nodes; }
        const std::vector<ModelCamera>& GetCameras() const { return m_Cameras; }
        const std::vector<ModelLight>&  GetLights()  const { return m_Lights; }

        // Per-node world matrices (parent-composed; nodes are topological). Empty when no node tree.
        std::vector<Mat4> ComputeNodeWorldTransforms() const;

        void Serialize(nlohmann::json& json) const;
        void Deserialize(const nlohmann::json& json);

    private:

        virtual void ProcessMeshData();

    protected:
        virtual ModelInfo GetModelInfo() const;
        void CacheModelInfo() { m_ModelInfo = GetModelInfo(); }
        ModelInfo m_ModelInfo;

        fs::path m_Path;
        std::vector<MeshData> m_MeshesData;
        std::vector<std::shared_ptr<Mesh>> m_Meshes;
        std::vector<UUID> m_Materials;

        bool m_IsSkinned = false;

        Skeleton m_Skeleton;
        std::vector<UUID> m_AnimationClipUUIDs;

        std::vector<ModelNode>   m_Nodes;
        std::vector<ModelCamera> m_Cameras;
        std::vector<ModelLight>  m_Lights;
    };
}
