#include "luthpch.h"
#include "ModelImporter.h"
#include "luth/core/Math.h"
#include <assimp/Importer.hpp>
#include <assimp/scene.h>
#include <assimp/postprocess.h>

namespace Luth
{
    static Mat4 AxisCorrectionMatrix(const aiScene* scene)
    {
        Mat4 correction = Mat4(1.0f);

        if (scene->mMetaData) {
            int upAxis = 1, frontAxis = 1;
            int upSign = 1, frontSign = 1;
            bool hasUp = false, hasFront = false;

            // Extract axis information from metadata
            hasUp = scene->mMetaData->Get("UpAxis", upAxis);
            hasFront = scene->mMetaData->Get("FrontAxis", frontAxis);

            // Handle sign
            if (hasUp) { upSign = upAxis >= 0 ? 1 : -1; upAxis = abs(upAxis); }
            if (hasFront) { frontSign = frontAxis >= 0 ? 1 : -1; frontAxis = abs(frontAxis); }

            // Common case: Convert Z-up to Y-up
            if (hasUp && upAxis == 2) {  // Z-up
                correction = glm::rotate(correction, glm::radians(-90.0f), Vec3(1.0f, 0.0f, 0.0f));
                // Adjust front axis if needed (convert from Y-forward to -Z-forward)
                if (hasFront && frontAxis == 1) {  // Y-front
                    correction = glm::rotate(correction, glm::radians(90.0f), Vec3(0.0f, 0.0f, 1.0f));
                }
            }

            // Handle coordinate system handedness if needed
            if (scene->mMetaData->HasKey("AxisMode")) {
                int axisMode;
                if (scene->mMetaData->Get("AxisMode", axisMode)) {
                    if (axisMode == 2) {  // Right-handed to left-handed
                        correction = glm::scale(correction, Vec3(-1.0f, 1.0f, 1.0f));
                    }
                }
            }
        }
        return correction;
    }

    static MeshData ProcessMesh(aiMesh* mesh, const aiScene* scene, const Mat4& transform)
    {
        MeshData data;
        data.Name = mesh->mName.C_Str();
        data.MaterialIndex = mesh->mMaterialIndex;

        // Vertices
        data.Vertices.reserve(mesh->mNumVertices);
        for (unsigned int i = 0; i < mesh->mNumVertices; i++) {
            Vertex vertex;
            
            // Position
            Vec4 pos = transform * Vec4(AiVec3ToGLM(mesh->mVertices[i]), 1.0f);
            vertex.Position = Vec3(pos);

            // Normal (Transform with Normal Matrix)
            Mat3 normalMatrix = ConvertToNormalMatrix(transform);
            if (mesh->HasNormals())
                vertex.Normal = glm::normalize(normalMatrix * AiVec3ToGLM(mesh->mNormals[i]));
            else
                vertex.Normal = Vec3(0.0f);

            // Texture Coordinates
            if (mesh->mTextureCoords[0]) {
                vertex.TexCoord0 = Vec2(mesh->mTextureCoords[0][i].x, mesh->mTextureCoords[0][i].y);
            } else {
                vertex.TexCoord0 = Vec2(0.0f);
            }

            // Tangents
            if (mesh->HasTangentsAndBitangents()) {
                vertex.Tangent = glm::normalize(normalMatrix * AiVec3ToGLM(mesh->mTangents[i]));
            } else {
                vertex.Tangent = Vec3(0.0f);
            }

            data.Vertices.push_back(vertex);
        }

        // Indices
        for (unsigned int i = 0; i < mesh->mNumFaces; i++) {
            aiFace face = mesh->mFaces[i];
            for (unsigned int j = 0; j < face.mNumIndices; j++)
                data.Indices.push_back(face.mIndices[j]);
        }

        return data;
    }

    static void ProcessNode(aiNode* node, const aiScene* scene, const Mat4& parentTransform, std::vector<MeshData>& outMeshes)
    {
        Mat4 transform = parentTransform * AiMat4ToGLM(node->mTransformation);

        for (unsigned int i = 0; i < node->mNumMeshes; i++) {
            aiMesh* mesh = scene->mMeshes[node->mMeshes[i]];
            outMeshes.push_back(ProcessMesh(mesh, scene, transform));
        }

        for (unsigned int i = 0; i < node->mNumChildren; i++) {
            ProcessNode(node->mChildren[i], scene, transform, outMeshes);
        }
    }

    bool ModelImporter::Import(const std::filesystem::path& path, std::unique_ptr<AssetData>& outData)
    {
        LH_PROFILE_FUNCTION();

        Assimp::Importer importer;
        const aiScene* scene = importer.ReadFile(path.string(),
            aiProcess_Triangulate | aiProcess_GenSmoothNormals | aiProcess_FlipUVs |
            aiProcess_CalcTangentSpace | aiProcess_JoinIdenticalVertices);

        if (!scene || scene->mFlags & AI_SCENE_FLAGS_INCOMPLETE || !scene->mRootNode) {
            LH_CORE_ERROR("ModelImporter: Failed to load model {0} : {1}", path.string(), importer.GetErrorString());
            return false;
        }

        auto modelData = std::make_unique<ModelAssetData>();
        ProcessNode(scene->mRootNode, scene, AxisCorrectionMatrix(scene), modelData->Meshes);

        outData = std::move(modelData);
        return true;
    }
}