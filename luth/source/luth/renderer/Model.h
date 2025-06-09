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
        glm::vec3 Position;
        glm::vec3 Normal;
        glm::vec2 TexCoord0;
        glm::vec2 TexCoord1;
        glm::vec3 Tangent;
    };

    struct MeshData {
        std::vector<Vertex> Vertices;
        std::vector<uint32_t> Indices;
        uint32_t MaterialIndex = 0;
        std::string Name;
    };

    class Model : public Resource
    {
    public:
        Model(const fs::path& path);
        virtual ~Model() = default;

        void Init();

        std::vector<MeshData>& GetMeshesData() { return m_MeshesData; }
        std::vector<std::shared_ptr<Mesh>>& GetMeshes() { return m_Meshes; }

        void AddMaterial(UUID uuid, u32 index) {
            if (index >= m_Materials.size()) {
                m_Materials.resize(index + 1);
            }
            m_Materials[index] = uuid;
        }
        std::vector<UUID>& GetMaterials() { return m_Materials; }

        bool IsSkinned() const { return m_IsSkinned; }
        void SetIsSkinned(bool value) { m_IsSkinned = value; }

        void Serialize(nlohmann::json& json) const;
        void Deserialize(const nlohmann::json& json);

    private:
        void LoadModel(const fs::path& path);
        void ProcessNode(aiNode* node, const aiScene* scene, const Mat4& parentTransform = Mat4(1.0f));
        MeshData ProcessMesh(aiMesh* mesh, const aiScene* scene, const Mat4& transform);
        Material ProcessMaterial(aiMaterial* material, const fs::path& directory);
        void LoadMaterials(const fs::path& path);
        Mat4 AxisCorrectionMatrix(const aiScene* scene);

        virtual void ProcessMeshData();

    protected:
        fs::path m_Path;
        std::vector<MeshData> m_MeshesData;
        std::vector<std::shared_ptr<Mesh>> m_Meshes;
        std::vector<UUID> m_Materials;

        bool m_IsSkinned = false;
    };
}
