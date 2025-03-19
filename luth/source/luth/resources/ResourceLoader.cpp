#include "luthpch.h"
#include "luth/resources/ResourceLoader.h"
#include "luth/renderer/Renderer.h"
#include "luth/renderer/RendererAPI.h"
//#include "luth/renderer/Material.h"
#include "luth/renderer/Model.h"
//#include "luth/renderer/openGL/GLMaterial.h"
#include "luth/renderer/openGL/GLMesh.h"
#include "luth/renderer/openGL/GLTexture.h"

namespace Luth
{
    //template<>
    //std::shared_ptr<Model> ResourceLoader::Load(const fs::path& path)
    //{
    //    Assimp::Importer importer;
    //    const aiScene* scene = importer.ReadFile(path.string(),
    //        aiProcess_Triangulate | aiProcess_GenSmoothNormals | aiProcess_FlipUVs | aiProcess_CalcTangentSpace);

    //    if (!scene || scene->mFlags & AI_SCENE_FLAGS_INCOMPLETE || !scene->mRootNode) {
    //        LH_CORE_ERROR("Model Loading Failed: {0}", path);
    //        return nullptr;
    //    }

    //    std::vector<std::shared_ptr<Mesh>> meshes;
    //    ProcessNode(scene->mRootNode, scene, path, meshes);

    //    return std::make_shared<Model>(meshes);
    //}

    //void ResourceLoader::ProcessNode(aiNode* node, const aiScene* scene, const fs::path& modelPath, std::vector<std::shared_ptr<Mesh>>& meshes)
    //{
    //    // Process all meshes in the current node
    //    for (unsigned int i = 0; i < node->mNumMeshes; i++) {
    //        aiMesh* ai_mesh = scene->mMeshes[node->mMeshes[i]];
    //        std::shared_ptr<Mesh> mesh = ProcessMesh(ai_mesh, scene, modelPath);
    //        if (mesh) {
    //            meshes.push_back(mesh);
    //        }
    //    }

    //    // Recursively process children nodes
    //    for (unsigned int i = 0; i < node->mNumChildren; i++) {
    //        ProcessNode(node->mChildren[i], scene, modelPath, meshes);
    //    }
    //}

    //std::shared_ptr<Mesh> ResourceLoader::ProcessMesh(aiMesh* ai_mesh, const aiScene* scene, const fs::path& modelPath)
    //{
    //    // Extract vertex data
    //    std::vector<float> vertices;
    //    std::vector<uint32_t> indices;

    //    // Process vertices
    //    for (unsigned int i = 0; i < ai_mesh->mNumVertices; i++) {
    //        // Position
    //        vertices.push_back(ai_mesh->mVertices[i].x);
    //        vertices.push_back(ai_mesh->mVertices[i].y);
    //        vertices.push_back(ai_mesh->mVertices[i].z);

    //        // Normals (always generated due to aiProcess_GenSmoothNormals)
    //        vertices.push_back(ai_mesh->mNormals[i].x);
    //        vertices.push_back(ai_mesh->mNormals[i].y);
    //        vertices.push_back(ai_mesh->mNormals[i].z);

    //        // Texture coordinates (first set only)
    //        if (ai_mesh->mTextureCoords[0]) {
    //            vertices.push_back(ai_mesh->mTextureCoords[0][i].x);
    //            vertices.push_back(ai_mesh->mTextureCoords[0][i].y);
    //        }
    //        else {
    //            vertices.push_back(0.0f);
    //            vertices.push_back(0.0f);
    //        }
    //    }

    //    // Process indices
    //    for (unsigned int i = 0; i < ai_mesh->mNumFaces; i++) {
    //        aiFace face = ai_mesh->mFaces[i];
    //        for (unsigned int j = 0; j < face.mNumIndices; j++) {
    //            indices.push_back(face.mIndices[j]);
    //        }
    //    }

    //    // Create vertex buffer
    //    auto vertexBuffer = std::make_shared<GLVertexBuffer>(
    //        vertices.data(),
    //        static_cast<uint32_t>(vertices.size() * sizeof(float))
    //    );

    //    // Define buffer layout
    //    BufferLayout layout = {
    //        { ShaderDataType::Float3, "a_Position" },
    //        { ShaderDataType::Float3, "a_Normal"   },
    //        { ShaderDataType::Float2, "a_TexCoord" }
    //    };
    //    vertexBuffer->SetLayout(layout);

    //    // Create index buffer
    //    auto indexBuffer = std::make_shared<GLIndexBuffer>(
    //        indices.data(),
    //        static_cast<uint32_t>(indices.size())
    //        );

    //    // Process material
    //    std::shared_ptr<Texture> texture;
    //    if (ai_mesh->mMaterialIndex >= 0) {
    //        aiMaterial* material = scene->mMaterials[ai_mesh->mMaterialIndex];
    //        texture = LoadMaterialTexture(material, aiTextureType_DIFFUSE, modelPath);
    //    }

    //    // Fallback to default texture if none found
    //    if (!texture) {
    //        texture = Load<Texture>("container.jpg");
    //    }

    //    return std::make_shared<Mesh>(vertexBuffer, indexBuffer, texture);
    //}

    //std::shared_ptr<Texture> ResourceLoader::LoadMaterialTexture(aiMaterial* mat, aiTextureType type, const fs::path& modelPath)
    //{
    //    if (mat->GetTextureCount(type) == 0)
    //        return nullptr;

    //    aiString aiPath;
    //    mat->GetTexture(type, 0, &aiPath);
    //    fs::path texturePath(aiPath.C_Str());

    //    // Extract filename and load through ResourceManager
    //    fs::path textureName = texturePath.filename().replace_extension("");
    //    return Load<Texture>(textureName);
    //}
}
