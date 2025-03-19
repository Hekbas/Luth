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
        ProcessNode(scene->mRootNode, scene);

        f32 tf = Time::GetTime();
        float loadTime = tf - ti;
        LH_CORE_INFO("Imported Model: {0}", path.string());
        LH_CORE_TRACE(" - In: {0}s", loadTime);
    }

    void Model::ProcessNode(aiNode* node, const aiScene* scene)
    {
        for (uint32_t i = 0; i < node->mNumMeshes; i++)
            m_Meshes.push_back(ProcessMesh(scene->mMeshes[node->mMeshes[i]], scene));

        for (uint32_t i = 0; i < node->mNumChildren; i++)
            ProcessNode(node->mChildren[i], scene);
    }

    MeshData Model::ProcessMesh(aiMesh* mesh, const aiScene* scene)
    {
        MeshData data;
        data.Vertices.reserve(mesh->mNumVertices);
        data.Indices.reserve(mesh->mNumFaces * 3);  // Assuming triangulation

        // Process vertices
        for (uint32_t i = 0; i < mesh->mNumVertices; i++) {
            Vertex vertex{};
            vertex.Position = { mesh->mVertices[i].x, mesh->mVertices[i].y, mesh->mVertices[i].z };

            if (mesh->mNormals)
                vertex.Normal = { mesh->mNormals[i].x, mesh->mNormals[i].y, mesh->mNormals[i].z };

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
                        case aiTextureType_DIFFUSE:     texType = TextureInfo::Type::Diffuse;  break;
                        case aiTextureType_SPECULAR:    texType = TextureInfo::Type::Specular; break;
                        case aiTextureType_NORMALS:     texType = TextureInfo::Type::Normal;   break;
                        case aiTextureType_BASE_COLOR:  texType = TextureInfo::Type::Diffuse;  break;
                        default: continue; // Skip unsupported types
                    }
                    material.Textures.push_back({ texType, directory / path.C_Str() });
                }
            }
        }
        return material;
    }
}
