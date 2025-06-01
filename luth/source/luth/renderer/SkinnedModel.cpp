#include "luthpch.h"
#include "luth/renderer/SkinnedModel.h"
#include "luth/resources/Resources.h"

#include <assimp/postprocess.h>
#include <assimp/scene.h>
#include <string>

namespace Luth
{
    using namespace glm;

    SkinnedModel::SkinnedModel(const fs::path& path) : Model(path)
    {
        m_Scene = m_Importer.ReadFile(path.string(),
            aiProcess_Triangulate | aiProcess_GenSmoothNormals | aiProcess_FlipUVs |
            aiProcess_CalcTangentSpace | aiProcess_JoinIdenticalVertices);

        if (!m_Scene || m_Scene->mFlags & AI_SCENE_FLAGS_INCOMPLETE || !m_Scene->mRootNode) {
            LH_CORE_ERROR("Failed to load model with animation: {0}", m_Importer.GetErrorString());
            return;
        }

        for (uint32_t i = 0; i < m_Scene->mNumMeshes; ++i) {
            aiMesh* mesh = m_Scene->mMeshes[i];
            std::vector<SkinnedVertex> skinnedVertices(mesh->mNumVertices);
            ExtractBoneWeights(mesh, skinnedVertices);
        }

        m_BoneInfo.resize(m_BoneCount);
    }

    void SkinnedModel::ExtractBoneWeights(aiMesh* mesh, std::vector<SkinnedVertex>& vertices)
    {
        for (uint32_t boneIndex = 0; boneIndex < mesh->mNumBones; ++boneIndex) {
            std::string boneName = mesh->mBones[boneIndex]->mName.C_Str();

            uint32_t index;
            if (m_BoneMapping.find(boneName) == m_BoneMapping.end()) {
                index = m_BoneCount++;
                m_BoneMapping[boneName] = index;
                BoneInfo boneInfo;
                boneInfo.offsetMatrix = AiMat4ToGLM(mesh->mBones[boneIndex]->mOffsetMatrix);
                m_BoneInfo.push_back(boneInfo);
            }
            else {
                index = m_BoneMapping[boneName];
            }

            for (uint32_t i = 0; i < mesh->mBones[boneIndex]->mNumWeights; ++i) {
                uint32_t vertexID = mesh->mBones[boneIndex]->mWeights[i].mVertexId;
                float weight = mesh->mBones[boneIndex]->mWeights[i].mWeight;

                for (int j = 0; j < 4; ++j) {
                    if (vertices[vertexID].weights[j] == 0.0f) {
                        vertices[vertexID].boneIDs[j] = index;
                        vertices[vertexID].weights[j] = weight;
                        break;
                    }
                }
            }
        }
    }

    void SkinnedModel::UpdateAnimation(float timeInSeconds)
    {
        if (!m_Scene || !m_Scene->mAnimations) return;

        const aiAnimation* animation = m_Scene->mAnimations[0];
        float ticksPerSecond = (float)(animation->mTicksPerSecond != 0.0 ? animation->mTicksPerSecond : 24.0);
        float timeInTicks = timeInSeconds * ticksPerSecond;
        float animationTime = fmod(timeInTicks, (float)animation->mDuration);

        ReadNodeHierarchy(animationTime, m_Scene->mRootNode, Mat4(1.0f));
    }

    void SkinnedModel::ReadNodeHierarchy(float animationTime, const aiNode* node, const Mat4& parentTransform)
    {
        std::string nodeName(node->mName.C_Str());
        const aiAnimation* animation = m_Scene->mAnimations[0];

        Mat4 nodeTransform = AiMat4ToGLM(node->mTransformation);
        const aiNodeAnim* nodeAnim = FindNodeAnim(animation, nodeName);

        if (nodeAnim) {
            // Interpolate position, rotation, scale
            // TODO: Add interpolation helpers if needed
        }

        Mat4 globalTransform = parentTransform * nodeTransform;

        if (m_BoneMapping.find(nodeName) != m_BoneMapping.end()) {
            uint32_t boneIndex = m_BoneMapping[nodeName];
            m_BoneInfo[boneIndex].finalTransform = globalTransform * m_BoneInfo[boneIndex].offsetMatrix;
        }

        for (uint32_t i = 0; i < node->mNumChildren; ++i) {
            ReadNodeHierarchy(animationTime, node->mChildren[i], globalTransform);
        }
    }

    const aiNodeAnim* SkinnedModel::FindNodeAnim(const aiAnimation* animation, const std::string& nodeName)
    {
        for (uint32_t i = 0; i < animation->mNumChannels; ++i) {
            if (nodeName == animation->mChannels[i]->mNodeName.C_Str())
                return animation->mChannels[i];
        }
        return nullptr;
    }
}
