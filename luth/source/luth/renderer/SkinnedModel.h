#pragma once

#include "luth/renderer/Model.h"
#include <glm/glm.hpp>

#include <assimp/Importer.hpp>

namespace Luth
{
    struct SkinnedVertex : public Vertex {
        glm::uvec4 boneIDs = glm::uvec4(0);
        glm::vec4 weights = glm::vec4(0.0f);
    };

    struct BoneInfo {
        Mat4 offsetMatrix;
        Mat4 finalTransform;
    };

    class SkinnedModel : public Model
    {
    public:
        SkinnedModel(const fs::path& path);

        void UpdateAnimation(float timeInSeconds);
        const std::vector<BoneInfo>& GetBoneTransforms() const { return m_BoneInfo; }

    private:
        void ExtractBoneWeights(aiMesh* mesh, std::vector<SkinnedVertex>& vertices);
        void ReadNodeHierarchy(float animationTime, const aiNode* node, const Mat4& parentTransform);
        const aiNodeAnim* FindNodeAnim(const aiAnimation* animation, const std::string& nodeName);

        std::unordered_map<std::string, uint32_t> m_BoneMapping;
        std::vector<BoneInfo> m_BoneInfo;
        uint32_t m_BoneCount = 0;

        Assimp::Importer m_Importer;
        const aiScene* m_Scene = nullptr;
    };
}
