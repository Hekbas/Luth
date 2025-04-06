#pragma once

#include "luth/renderer/Material.h"
#include "luth/renderer/Mesh.h"
#include "luth/resources/Resource.h"

#include <string>
#include <vector>
#include <memory>
#include <filesystem>
#include <assimp/scene.h>

namespace Luth
{
    struct Vertex {
        glm::vec3 position;
        glm::vec3 normal;
        glm::vec2 texCoord0;
        glm::vec2 texCoord1;
        glm::vec3 tangent;
    };

    struct MeshData {
        std::vector<Vertex> vertices;
        std::vector<uint32_t> indices;
        uint32_t materialIndex = 0;
        std::string name;
    };

    class Model : public Resource
    {
    public:
        Model(const fs::path& path);

        std::vector<MeshData>& GetMeshesData() { return m_MeshesData; }
        std::vector<std::shared_ptr<Mesh>>& GetMeshes() { return m_Meshes; }
        std::vector<Material>& GetMaterials() { return m_Materials; }

    private:
        void LoadModel(const fs::path& path);
        void ProcessNode(aiNode* node, const aiScene* scene, const Mat4& parentTransform = Mat4(1.0f));
        MeshData ProcessMesh(aiMesh* mesh, const aiScene* scene, const Mat4& transform);
        Material ProcessMaterial(aiMaterial* material, const fs::path& directory);
        Mat4 AxisCorrectionMatrix(const aiScene* scene);

        void ProcessMeshData();

        std::vector<MeshData> m_MeshesData;
        std::vector<std::shared_ptr<Mesh>> m_Meshes;
        std::vector<Material> m_Materials;
        fs::path m_Directory;
    };
}
