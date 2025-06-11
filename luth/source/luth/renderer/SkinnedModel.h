#pragma once

#include "luth/renderer/Model.h"
#include <glm/glm.hpp>

#include <assimp/Importer.hpp>

namespace Luth
{
    struct SkinnedVertex : public Vertex {
        glm::ivec4 BoneIDs = glm::ivec4(-1);
        glm::vec4 BoneWeights = glm::vec4(0.0f);
    };

    struct BoneInfo {
        Mat4 OffsetMatrix = glm::mat4(1.0f);;
        Mat4 FinalTransform = glm::mat4(1.0f);
    };

    class SkinnedModel : public Model
    {
    public:
        SkinnedModel(const fs::path& path);
        ~SkinnedModel() = default;

        void ProcessMeshData() override;

        void UpdateAnimation(float timeInSeconds, i32 animationIndex);

        const std::vector<BoneInfo>& GetBoneTransforms() const { return m_BoneInfo; }
        std::vector<Mat4> GetFinalTransforms() const {
            std::vector<Mat4> transforms;
            transforms.reserve(m_BoneInfo.size());
            for (const auto& bone : m_BoneInfo) {
                transforms.push_back(bone.FinalTransform);
            }
            return transforms;
        }

		const glm::mat4& GetGlobalInverseTransform() const { return m_GlobalInverseTransform; }

    private:
        void ExtractBoneWeights(aiMesh* mesh, std::vector<SkinnedVertex>& vertices);

        inline void SetVertexBoneData(SkinnedVertex& vert, int boneID, float weight)
        {
            for (int i = 0; i < 4; i++) {
                if (vert.BoneIDs[i] < 0) {
                    vert.BoneIDs[i] = (float)boneID;
                    vert.BoneWeights[i] = weight;
                    break;
                }
            }
        }

        const aiNodeAnim* FindNodeAnim(const aiAnimation* animation, const std::string& nodeName);

        aiVector3D InterpolatePosition(float animationTime, const aiNodeAnim* nodeAnim);
        uint32_t FindPosition(float animationTime, const aiNodeAnim* nodeAnim);

        aiQuaternion InterpolateRotation(float animationTime, const aiNodeAnim* nodeAnim);
        uint32_t FindRotation(float animationTime, const aiNodeAnim* nodeAnim);

        aiVector3D InterpolateScaling(float animationTime, const aiNodeAnim* nodeAnim);
        uint32_t FindScaling(float animationTime, const aiNodeAnim* nodeAnim);

        void ReadNodeHierarchy(const aiAnimation* animation, float animationTime, const aiNode* node, const glm::mat4& parentTransform);

        Assimp::Importer m_Importer;
        const aiScene* m_Scene = nullptr;

        glm::mat4 m_GlobalInverseTransform;
        std::vector<std::vector<SkinnedVertex>> m_SkinnedVertices;
        std::unordered_map<std::string, uint32_t> m_BoneMapping;
        std::vector<BoneInfo> m_BoneInfo;
        uint32_t m_BoneCount = 0;


        // DEBUG SKELETON 
        struct BoneNode {
            std::string Name;
            Mat4 Transformation;
            int ParentIndex = -1;
            std::vector<uint32_t> Children;
            uint32_t BoneIndex = -1; // Index in m_BoneInfo, -1 if not a bone
        };

    public:
        const std::vector<BoneNode>& GetBoneHierarchy() const { return m_BoneHierarchy; }
        uint32_t GetRootNodeIndex() const { return m_RootNodeIndex; }

    private:
		void BuildBoneHierarchy(const aiNode* node, int parentIndex);

        std::vector<BoneNode> m_BoneHierarchy;
        uint32_t m_RootNodeIndex = 0;
    };
}
