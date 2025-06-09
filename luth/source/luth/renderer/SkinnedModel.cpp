#include "luthpch.h"
#include "luth/renderer/SkinnedModel.h"
#include "luth/resources/Resources.h"

#include <assimp/postprocess.h>
#include <glm/ext/matrix_integer.hpp>
#include <assimp/scene.h>
#include <string>

namespace Luth
{
    using namespace glm;

    SkinnedModel::SkinnedModel(const fs::path& path) : Model(path)
    {
        m_Importer.SetPropertyBool(AI_CONFIG_IMPORT_FBX_PRESERVE_PIVOTS, false);

        m_Scene = m_Importer.ReadFile(path.string(),
            aiProcess_Triangulate | aiProcess_GenSmoothNormals | aiProcess_FlipUVs |
            aiProcess_CalcTangentSpace | aiProcess_JoinIdenticalVertices);

        if (!m_Scene || m_Scene->mFlags & AI_SCENE_FLAGS_INCOMPLETE || !m_Scene->mRootNode) {
            LH_CORE_ERROR("Failed to load model with animation: {0}", m_Importer.GetErrorString());
            return;
        }

        m_GlobalInverseTransform = glm::inverse(AiMat4ToGLM(m_Scene->mRootNode->mTransformation));

        // Convert vertices to skinned version
        for (uint32_t i = 0; i < m_Scene->mNumMeshes; ++i) {
            aiMesh* mesh = m_Scene->mMeshes[i];

            // Create skinned vertices from base vertices
            std::vector<SkinnedVertex> skinnedVertices;
            skinnedVertices.reserve(m_MeshesData[i].Vertices.size());

            for (const Vertex& v : m_MeshesData[i].Vertices) {
                SkinnedVertex sv;
                // Copy base data
                sv.Position  = v.Position;
                sv.Normal    = v.Normal;
                sv.TexCoord0 = v.TexCoord0;
                sv.TexCoord1 = v.TexCoord1;
                sv.Tangent   = v.Tangent;
                // Initialize bone data
                sv.BoneIDs = glm::ivec4(-1);
                sv.BoneWeights = glm::vec4(0.0f);
                skinnedVertices.push_back(sv);
            }

            // Add bone weights
            ExtractBoneWeights(mesh, skinnedVertices);

            m_MeshesData[i].Vertices.clear();
            m_MeshesData[i].Vertices.reserve(skinnedVertices.size());
            for (auto& sv : skinnedVertices) {
                // Store as base Vertex - handle this in ProcessMeshData
                m_MeshesData[i].Vertices.push_back(static_cast<Vertex>(sv));
            }

            // Store extended data separately
            m_SkinnedVertices.push_back(std::move(skinnedVertices));
        }

        // Log m_BoneMapping
        for (const auto& [boneName, index] : m_BoneMapping) {
            LH_CORE_TRACE("{0:<4} | {1}", index, boneName);
        }

        // Build bone hierarchy
        LH_CORE_INFO("Building bone hierarchy...");
        BuildBoneHierarchy(m_Scene->mRootNode, -1);

        // Log hierarchy summary
        LH_CORE_INFO("Bone hierarchy built successfully");
        LH_CORE_INFO("Total nodes in hierarchy: {0}", m_BoneHierarchy.size());
        LH_CORE_INFO("Total bones: {0}", m_BoneCount);

        // Log hierarchy structure
        if (m_BoneHierarchy.empty()) {
            LH_CORE_WARN("No bone hierarchy to log");
            return;
        }

        // Recursive lambda to print hierarchy
        std::function<void(uint32_t, int)> printNode = [&](uint32_t nodeIndex, int depth) {
            const auto& node = m_BoneHierarchy[nodeIndex];

            // Create indentation
            std::string indent(depth * 2, ' ');

            // Format bone info
            std::string boneInfo = node.Name;
            if (node.BoneIndex != -1) {
                boneInfo += " (Bone ID: " + std::to_string(node.BoneIndex) + ")";
            }

            // Log the node
            LH_CORE_TRACE("{0}- {1}", indent, boneInfo);

            // Recursively print children
            for (const auto& childNode : m_BoneHierarchy) {
                if (childNode.ParentIndex == static_cast<int>(nodeIndex)) {
                    printNode(&childNode - &m_BoneHierarchy[0], depth + 1);
                }
            }
        };

        LH_CORE_TRACE("Bone Hierarchy:");
        printNode(m_RootNodeIndex, 0);
    }

    void SkinnedModel::BuildBoneHierarchy(const aiNode* node, int parentIndex)
    {
        std::string nodeName(node->mName.C_Str());

        BoneNode boneNode;
        boneNode.Name = nodeName;
        boneNode.Transformation = AiMat4ToGLM(node->mTransformation);
        boneNode.ParentIndex = parentIndex;

        // Check if this node is a bone
        auto it = m_BoneMapping.find(nodeName);
        if (it != m_BoneMapping.end()) {
            boneNode.BoneIndex = it->second;
        }

        uint32_t nodeIndex = static_cast<uint32_t>(m_BoneHierarchy.size());
        m_BoneHierarchy.push_back(boneNode);

        // Set as root if it's the scene root
        if (parentIndex == -1) {
            m_RootNodeIndex = nodeIndex;
        }
        // Add this node to parent's children list
        else if (parentIndex >= 0) {
            m_BoneHierarchy[parentIndex].Children.push_back(nodeIndex);
        }

        // Process children recursively
        for (uint32_t i = 0; i < node->mNumChildren; i++) {
            BuildBoneHierarchy(node->mChildren[i], nodeIndex);
        }
    }

    void SkinnedModel::ExtractBoneWeights(aiMesh* mesh, std::vector<SkinnedVertex>& vertices)
    {
        LH_CORE_TRACE("Extracting bone weights for mesh: {0}", mesh->mName.C_Str());

        for (uint32_t boneIndex = 0; boneIndex < mesh->mNumBones; ++boneIndex) {
            aiBone* bone = mesh->mBones[boneIndex];
            std::string boneName = bone->mName.C_Str();

            // Get or assign a new index for this bone
            uint32_t boneID;
            if (m_BoneMapping.find(boneName) == m_BoneMapping.end()) {
                boneID = m_BoneCount++;
                m_BoneMapping[boneName] = boneID;
                BoneInfo boneInfo;
                boneInfo.OffsetMatrix = AiMat4ToGLM(bone->mOffsetMatrix);
                m_BoneInfo.push_back(boneInfo);
            }
            else {
                boneID = m_BoneMapping[boneName];
            }

            // Set id / weights
            for (uint32_t w = 0; w < bone->mNumWeights; ++w) {
                uint32_t vertexID = bone->mWeights[w].mVertexId;
                float weight = bone->mWeights[w].mWeight;
                SetVertexBoneData(vertices[vertexID], boneID, weight);
            }
        }

        // Normalize vertex weights
        for (auto& vert : vertices) {
            float total = vert.BoneWeights.x
                + vert.BoneWeights.y
                + vert.BoneWeights.z
                + vert.BoneWeights.w;
            if (total > 0.0f) {
                vert.BoneWeights /= total;
            }
        }
    }

    void SkinnedModel::ProcessMeshData()
    {
        for (size_t i = 0; i < m_MeshesData.size(); ++i) {
            auto& meshData = m_MeshesData[i];

            auto vb = VertexBuffer::Create(m_SkinnedVertices[i].data(), m_SkinnedVertices[i].size() * sizeof(SkinnedVertex));
            vb->SetLayout({
                { ShaderDataType::Float3, "a_Position"    },
                { ShaderDataType::Float3, "a_Normal"      },
                { ShaderDataType::Float2, "a_TexCoord0"   },
                { ShaderDataType::Float2, "a_TexCoord1"   },
                { ShaderDataType::Float3, "a_Tangent"     },
                { ShaderDataType::Int4,   "a_BoneIDs"     },
                { ShaderDataType::Float4, "a_BoneWeights" }
            });

            auto ib = IndexBuffer::Create(meshData.Indices.data(), meshData.Indices.size());
            m_Meshes.push_back(Mesh::Create(vb, ib));
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
            aiVector3D translation = InterpolatePosition(animationTime, nodeAnim);
            Mat4 translationMatrix = glm::translate(Mat4(1.0f), AiVec3ToGLM(translation));

            aiQuaternion rotation = InterpolateRotation(animationTime, nodeAnim);
            Mat4 rotationMatrix = glm::toMat4(AiQuatToGLM(rotation));

            aiVector3D scale = InterpolateScaling(animationTime, nodeAnim);
            Mat4 scaleMatrix = glm::scale(Mat4(1.0f), AiVec3ToGLM(scale));

            nodeTransform = translationMatrix * rotationMatrix * scaleMatrix;
        }

        Mat4 globalTransform = parentTransform * nodeTransform;

        if (m_BoneMapping.find(nodeName) != m_BoneMapping.end()) {
            uint32_t boneIndex = m_BoneMapping[nodeName];
            m_BoneInfo[boneIndex].FinalTransform =
				m_GlobalInverseTransform *
                globalTransform *
                m_BoneInfo[boneIndex].OffsetMatrix;
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

    aiVector3D SkinnedModel::InterpolatePosition(float animationTime, const aiNodeAnim* nodeAnim)
    {
        if (nodeAnim->mNumPositionKeys == 1) {
            return nodeAnim->mPositionKeys[0].mValue;
        }

        uint32_t positionIndex = FindPosition(animationTime, nodeAnim);
        uint32_t nextPositionIndex = positionIndex + 1;
        float deltaTime = nodeAnim->mPositionKeys[nextPositionIndex].mTime -
            nodeAnim->mPositionKeys[positionIndex].mTime;
        float factor = (animationTime - nodeAnim->mPositionKeys[positionIndex].mTime) / deltaTime;

        const aiVector3D& start = nodeAnim->mPositionKeys[positionIndex].mValue;
        const aiVector3D& end = nodeAnim->mPositionKeys[nextPositionIndex].mValue;
        return start + factor * (end - start);
    }

    uint32_t SkinnedModel::FindPosition(float animationTime, const aiNodeAnim* nodeAnim)
    {
        for (uint32_t i = 0; i + 1 < nodeAnim->mNumPositionKeys; i++) {
            if (animationTime < nodeAnim->mPositionKeys[i + 1].mTime)
                return i;
        }
        return nodeAnim->mNumPositionKeys > 1 ? nodeAnim->mNumPositionKeys - 2 : 0;
    }

    aiQuaternion SkinnedModel::InterpolateRotation(float animationTime, const aiNodeAnim* nodeAnim)
    {
        if (nodeAnim->mNumRotationKeys == 1) {
            return nodeAnim->mRotationKeys[0].mValue;
        }

        uint32_t rotationIndex = FindRotation(animationTime, nodeAnim);
        uint32_t nextRotationIndex = rotationIndex + 1;
        float deltaTime = nodeAnim->mRotationKeys[nextRotationIndex].mTime -
            nodeAnim->mRotationKeys[rotationIndex].mTime;
        float factor = (animationTime - nodeAnim->mRotationKeys[rotationIndex].mTime) / deltaTime;

        const aiQuaternion& start = nodeAnim->mRotationKeys[rotationIndex].mValue;
        const aiQuaternion& end = nodeAnim->mRotationKeys[nextRotationIndex].mValue;

        aiQuaternion result;
        aiQuaternion::Interpolate(result, start, end, factor);
        return result.Normalize();
    }

    uint32_t SkinnedModel::FindRotation(float animationTime, const aiNodeAnim* nodeAnim)
    {
        for (uint32_t i = 0; i + 1 < nodeAnim->mNumRotationKeys; i++) {
            if (animationTime < nodeAnim->mRotationKeys[i + 1].mTime)
                return i;
        }
        return nodeAnim->mNumRotationKeys > 1 ? nodeAnim->mNumRotationKeys - 2 : 0;
    }

    aiVector3D SkinnedModel::InterpolateScaling(float animationTime, const aiNodeAnim* nodeAnim)
    {
        if (nodeAnim->mNumScalingKeys == 1) {
            return nodeAnim->mScalingKeys[0].mValue;
        }

        uint32_t scalingIndex = FindScaling(animationTime, nodeAnim);
        uint32_t nextScalingIndex = scalingIndex + 1;
        float deltaTime = nodeAnim->mScalingKeys[nextScalingIndex].mTime -
            nodeAnim->mScalingKeys[scalingIndex].mTime;
        float factor = (animationTime - nodeAnim->mScalingKeys[scalingIndex].mTime) / deltaTime;

        const aiVector3D& start = nodeAnim->mScalingKeys[scalingIndex].mValue;
        const aiVector3D& end = nodeAnim->mScalingKeys[nextScalingIndex].mValue;
        return start + factor * (end - start);
    }

    uint32_t SkinnedModel::FindScaling(float animationTime, const aiNodeAnim* nodeAnim)
    {
        for (uint32_t i = 0; i + 1 < nodeAnim->mNumScalingKeys; i++) {
            if (animationTime < nodeAnim->mScalingKeys[i + 1].mTime)
                return i;
        }
        return nodeAnim->mNumScalingKeys > 1 ? nodeAnim->mNumScalingKeys - 2 : 0;
    }
}
