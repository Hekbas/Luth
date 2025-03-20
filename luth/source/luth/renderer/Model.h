#pragma once

#include "luth/renderer/Mesh.h"
#include "luth/renderer/openGL/GLMesh.h"

#include <memory>
#include <vector>
#include <filesystem>
#include <assimp/scene.h>

namespace Luth
{
    struct Vertex {
        glm::vec3 Position;
        glm::vec3 Normal;
        glm::vec2 TexCoords;
    };

    struct MeshData {
        std::vector<Vertex> Vertices;
        std::vector<uint32_t> Indices;
        uint32_t MaterialIndex = 0;
    };

    struct TextureInfo {
        enum class Type { Diffuse, Normal, Emissive, Metalness, Roughness };
        Type type;
        fs::path path;
        u32 id;
    };

    struct Material {
        std::vector<TextureInfo> Textures;
    };

    class Model
    {
    public:
        Model(const fs::path& path);

        const std::vector<MeshData>& GetMeshes() const { return m_Meshes; }
        const std::vector<Material>& GetMaterials() const { return m_Materials; }

    private:
        void LoadModel(const fs::path& path);
        void ProcessNode(aiNode* node, const aiScene* scene, const Mat4& parentTransform = Mat4(1.0f));
        MeshData ProcessMesh(aiMesh* mesh, const aiScene* scene, const Mat4& transform);
        Material ProcessMaterial(aiMaterial* material, const fs::path& directory);

        Mat4 AxisCorrectionMatrix(const aiScene* scene);

        std::vector<MeshData> m_Meshes;
        std::vector<Material> m_Materials;
        fs::path m_Directory;
    };
}
