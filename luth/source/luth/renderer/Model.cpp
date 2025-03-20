#include "luthpch.h"
#include "luth/renderer/Model.h"

#include <assimp/Importer.hpp>
#include <assimp/postprocess.h>

namespace Luth
{
    Model::Model(const fs::path& path) { LoadModel(path); }

    void Model::LoadModel(const fs::path& path)
    {
        f32 ti = Time::GetTime();

        Assimp::Importer importer;
        const aiScene* scene = importer.ReadFile(path.string(),
            aiProcess_Triangulate | aiProcess_GenNormals | aiProcess_FlipUVs);

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
        data.Vertices.reserve(mesh->mNumVertices);
        data.Indices.reserve(mesh->mNumFaces * 3);  // Assuming triangulation

        const Mat3 normalMatrix = ConvertToNormalMatrix(transform);

        // Process vertices with transform
        for (uint32_t i = 0; i < mesh->mNumVertices; i++) {
            Vertex vertex;
            const aiVector3D& pos = mesh->mVertices[i];

            const Vec4 transformedPos = transform * Vec4(pos.x, pos.y, pos.z, 1.0f);
            vertex.Position = Vec3(transformedPos);

            if (mesh->mNormals) {
                const aiVector3D& norm = mesh->mNormals[i];
                const Vec3 transformedNorm = normalMatrix * Vec3(norm.x, norm.y, norm.z);
                vertex.Normal = glm::normalize(transformedNorm);
            }

            if (mesh->mTextureCoords[0])
                vertex.TexCoords = { mesh->mTextureCoords[0][i].x, mesh->mTextureCoords[0][i].y };

            data.Vertices.push_back(vertex);
        }

        // Process indices
        for (uint32_t i = 0; i < mesh->mNumFaces; i++) {
            const aiFace& face = mesh->mFaces[i];
            for (uint32_t j = 0; j < face.mNumIndices; j++) {
                data.Indices.push_back(static_cast<uint32_t>(face.mIndices[j]));
            }
        }

        aiString name = mesh->mName;

        // Process material
        if (mesh->mMaterialIndex >= 0 && static_cast<uint32_t>(mesh->mMaterialIndex) < scene->mNumMaterials) {
            data.MaterialIndex = static_cast<uint32_t>(m_Materials.size());
            m_Materials.push_back(ProcessMaterial(scene->mMaterials[mesh->mMaterialIndex], m_Directory));
        }

        return data;
    }

    Material Model::ProcessMaterial(aiMaterial* mat, const fs::path& directory)
    {
        Material material;
        aiString path;
        for (uint32_t type = aiTextureType_DIFFUSE; type <= aiTextureType_UNKNOWN; type++) {
            aiTextureType aiType = static_cast<aiTextureType>(type);
            uint32_t count = mat->GetTextureCount(aiType);
            for (uint32_t i = 0; i < count; i++) {
                if (mat->GetTexture(aiType, i, &path) == AI_SUCCESS) {
                    TextureInfo::Type texType;
                    switch (aiType) {
                        case aiTextureType_DIFFUSE:   texType = TextureInfo::Type::Diffuse;   break;
                        case aiTextureType_NORMALS:   texType = TextureInfo::Type::Normal;    break;
                        case aiTextureType_HEIGHT:    texType = TextureInfo::Type::Normal;    break;
                        case aiTextureType_EMISSIVE:  texType = TextureInfo::Type::Emissive;  break;
                        case aiTextureType_METALNESS: texType = TextureInfo::Type::Metalness; break;
                        case aiTextureType_SHININESS: texType = TextureInfo::Type::Roughness; break;
                        default: continue; // Skip unsupported types
                    }
                    material.Textures.push_back({ texType, directory / path.C_Str() });
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
