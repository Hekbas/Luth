#include "luthpch.h"
#include "luth/renderer/Model.h"

#include <assimp/Importer.hpp>
#include <assimp/postprocess.h>
#include <string>

namespace Luth
{
    Model::Model(const fs::path& path) { LoadModel(path); }

    void Model::LoadModel(const fs::path& path)
    {
        f32 ti = Time::GetTime();

        Assimp::Importer importer;
        const aiScene* scene = importer.ReadFile(path.string(),
            aiProcess_Triangulate | aiProcess_GenNormals | aiProcess_FlipUVs | aiProcess_CalcTangentSpace);

        if (!scene || scene->mFlags & AI_SCENE_FLAGS_INCOMPLETE || !scene->mRootNode) {
            LH_CORE_ERROR("{0}", importer.GetErrorString());
            return;
        }

        m_Directory = path.parent_path();
        ProcessNode(scene->mRootNode, scene, AxisCorrectionMatrix(scene));

        f32 tf = Time::GetTime();
        float loadTime = tf - ti;
        LH_CORE_INFO("Imported Model: {0}", path.string());
        LH_CORE_TRACE(" - In: {0}s", loadTime);
    }

    void Model::ProcessNode(aiNode* node, const aiScene* scene, const Mat4& parentTransform)
    {
        // Calculate current node transform
        const Mat4 nodeTransform = parentTransform * AiMat4ToGLM(node->mTransformation);

        // Process meshes with current transform
        for (uint32_t i = 0; i < node->mNumMeshes; i++) {
            aiMesh* mesh = scene->mMeshes[node->mMeshes[i]];
            m_Meshes.push_back(ProcessMesh(mesh, scene, nodeTransform));
        }

        // Process children recursively
        for (uint32_t i = 0; i < node->mNumChildren; i++) {
            ProcessNode(node->mChildren[i], scene, nodeTransform);
        }
    }

    MeshData Model::ProcessMesh(aiMesh* mesh, const aiScene* scene, const Mat4& transform)
    {
        MeshData data;
        data.vertices.reserve(mesh->mNumVertices);
        data.indices.reserve(mesh->mNumFaces * 3);  // Assuming triangulation

        const Mat3 normalMatrix = ConvertToNormalMatrix(transform);

        // Process vertices with transform
        for (uint32_t i = 0; i < mesh->mNumVertices; i++) {
            Vertex vertex{};
            const aiVector3D& pos = mesh->mVertices[i];

            const Vec4 transformedPos = transform * Vec4(pos.x, pos.y, pos.z, 1.0f);
            vertex.position = Vec3(transformedPos);

            if (mesh->mNormals) {
                const aiVector3D& norm = mesh->mNormals[i];
                const Vec3 transformedNorm = normalMatrix * Vec3(norm.x, norm.y, norm.z);
                vertex.normal = glm::normalize(transformedNorm);
            }

            if (mesh->mTextureCoords[0])
                vertex.texCoord0 = { mesh->mTextureCoords[0][i].x, mesh->mTextureCoords[0][i].y };
            if (mesh->mTextureCoords[1])
                vertex.texCoord1 = { mesh->mTextureCoords[1][i].x, mesh->mTextureCoords[1][i].y };

            if (mesh->mTangents) {
                const aiVector3D& tan = mesh->mTangents[i];
                const Vec3 transformedTangent = normalMatrix * Vec3(tan.x, tan.y, tan.z);
                vertex.tangent = glm::normalize(transformedTangent);
            }

            data.vertices.push_back(vertex);
        }

        // Process indices
        for (uint32_t i = 0; i < mesh->mNumFaces; i++) {
            const aiFace& face = mesh->mFaces[i];
            for (uint32_t j = 0; j < face.mNumIndices; j++) {
                data.indices.push_back(static_cast<uint32_t>(face.mIndices[j]));
            }
        }

        aiString name = mesh->mName;

        // Process material
        if (mesh->mMaterialIndex >= 0 && static_cast<uint32_t>(mesh->mMaterialIndex) < scene->mNumMaterials) {
            data.materialIndex = static_cast<uint32_t>(m_Materials.size());
            m_Materials.push_back(ProcessMaterial(scene->mMaterials[mesh->mMaterialIndex], m_Directory));
        }

        return data;
    }

    Material Model::ProcessMaterial(aiMaterial* mat, const fs::path& directory)
    {
        Material material;
        TextureInfo texInfo;
        aiString path;
        for (uint32_t type = aiTextureType_DIFFUSE; type <= aiTextureType_UNKNOWN; type++)
        {
            aiTextureType aiType = static_cast<aiTextureType>(type);
            for (uint32_t i = 0; i < mat->GetTextureCount(aiType); i++)
            {
                if (mat->GetTexture(aiType, i, &path, nullptr, &texInfo.uvIndex) == AI_SUCCESS) {
                    switch (aiType) {
                        case aiTextureType_DIFFUSE:         texInfo.type = TextureType::Diffuse;   break;
                        case aiTextureType_BASE_COLOR:      texInfo.type = TextureType::Diffuse;   break;
                        case aiTextureType_NORMALS:         texInfo.type = TextureType::Normal;    break;
                        case aiTextureType_NORMAL_CAMERA:   texInfo.type = TextureType::Normal;    break;
                        case aiTextureType_HEIGHT:          texInfo.type = TextureType::Normal;    break;
                        case aiTextureType_EMISSIVE:        texInfo.type = TextureType::Emissive;  break;
                        case aiTextureType_EMISSION_COLOR:  texInfo.type = TextureType::Emissive;  break;
                        case aiTextureType_METALNESS:       texInfo.type = TextureType::Metalness; break;
                        case aiTextureType_SHININESS:       texInfo.type = TextureType::Roughness; break;
                        default: continue; // Skip unsupported types
                    }
                    fs::path name = path.C_Str();
                    auto result = ResourceManager::FindResources(Resource::Model, name.filename().stem().string() + ".*", true);
                    if (!result.empty()) texInfo.path = result[0];
                    material.AddTexture(texInfo);
                }
            }
        }
        return material;
    }

    Mat4 Model::AxisCorrectionMatrix(const aiScene* scene)
    {
        Mat4 correction = Mat4(1.0f);

        if (scene->mMetaData) {
            int upAxis = 1, frontAxis = 1;
            int upSign = 1, frontSign = 1;
            bool hasUp = false, hasFront = false;

            // Extract axis information from metadata
            hasUp = scene->mMetaData->Get("UpAxis", upAxis);
            hasFront = scene->mMetaData->Get("FrontAxis", frontAxis);

            // Handle sign (assuming negative values indicate negative direction)
            if (hasUp) {
                upSign = upAxis >= 0 ? 1 : -1;
                upAxis = abs(upAxis);
            }
            if (hasFront) {
                frontSign = frontAxis >= 0 ? 1 : -1;
                frontAxis = abs(frontAxis);
            }

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

        // Fallback for common Z-up to Y-up conversion
        else {
            correction = glm::rotate(Mat4(1.0f), glm::radians(-90.0f), Vec3(1.0f, 0.0f, 0.0f));
        }

        return correction;
    }
}
