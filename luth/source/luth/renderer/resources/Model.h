#pragma once

#include "luth/core/types/LuthMath.h"
#include "luth/renderer/material/Material.h"
#include "luth/renderer/resources/Mesh.h"
#include "luth/animation/Skeleton.h"
#include "luth/animation/AnimationClip.h"
#include "luth/resources/Asset.h"

#include <string>
#include <vector>
#include <memory>
#include <filesystem>

namespace Luth
{
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

    class Model : public Asset
    {
    public:
        struct CreateParams; // Forward decl

        virtual AssetType GetType() const override { return AssetType::Model; }
        
        Model() = default;
        virtual ~Model() = default;

        static std::shared_ptr<Model> Create(const std::vector<MeshData>& meshData, const std::vector<UUID>& materials);
        static std::shared_ptr<Model> Create(const std::vector<MeshData>& meshData, const std::vector<UUID>& materials,
            const Skeleton& skeleton, const std::vector<AnimationClip>& clips, bool isSkinned);
        
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

        // Skeleton & Animation
        const Skeleton& GetSkeleton() const { return m_Skeleton; }
        Skeleton& GetSkeleton() { return m_Skeleton; }
        const std::vector<AnimationClip>& GetAnimationClips() const { return m_AnimationClips; }
        std::vector<AnimationClip>& GetAnimationClips() { return m_AnimationClips; }
        const AnimationClip* GetAnimationClip(u32 index) const {
            return (index < m_AnimationClips.size()) ? &m_AnimationClips[index] : nullptr;
        }

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
        std::vector<AnimationClip> m_AnimationClips;
    };
}
