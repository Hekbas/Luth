#include "luthpch.h"
#include "luth/renderer/Model.h"
#include "luth/renderer/Renderer.h"
#include "luth/core/Log.h"
#include "luth/renderer/backend/vulkan/VulkanBuffer.h"

namespace Luth
{
    std::shared_ptr<Model> Model::Create(const std::vector<MeshData>& meshData)
    {
        auto model = std::make_shared<Model>();
        model->m_MeshesData = meshData;
        model->ProcessMeshData();
        return model;
    }

    void Model::ProcessMeshData()
    {
        // Upload to GPU (Main Thread)
        for (const auto& data : m_MeshesData)
        {
            auto vb = VertexBuffer::Create(data.Vertices.data(), data.Vertices.size() * sizeof(Vertex));
            
            vb->SetLayout({
                { ShaderDataType::Float3, "a_Position"  },
                { ShaderDataType::Float3, "a_Normal"    },
                { ShaderDataType::Float2, "a_TexCoord0" },
                { ShaderDataType::Float2, "a_TexCoord1" },
                { ShaderDataType::Float3, "a_Tangent"   } 
            });

            auto ib = IndexBuffer::Create(data.Indices.data(), data.Indices.size());
            m_Meshes.push_back(Mesh::Create(vb, ib));
        }
        
        CacheModelInfo();
    }

    ModelInfo Model::GetModelInfo() const
    {
        ModelInfo info;
        // info.Path = m_Path; // Path is managed by AssetDatabase
        info.IsSkinned = m_IsSkinned;
        info.TotalMeshCount = (u32)m_Meshes.size();
        info.MaterialCount = (u32)m_Materials.size();
        
        // Calculate totals
        for (const auto& meshData : m_MeshesData) {
            info.TotalVertexCount += static_cast<uint32_t>(meshData.Vertices.size());
            info.TotalIndexCount += static_cast<uint32_t>(meshData.Indices.size());

            MeshInfo meshInfo;
            meshInfo.Name = meshData.Name;
            meshInfo.VertexCount = static_cast<uint32_t>(meshData.Vertices.size());
            meshInfo.IndexCount = static_cast<uint32_t>(meshData.Indices.size());
            meshInfo.MaterialIndex = meshData.MaterialIndex;
            info.Meshes.push_back(meshInfo);
        }

        return info;
    }

    void Model::Serialize(nlohmann::json& json) const
    {
        nlohmann::json materials_json;
        for (size_t i = 0; i < m_Materials.size(); ++i) {
            if (m_Materials[i].IsValid()) {
                materials_json[std::to_string(i)] = m_Materials[i].ToString();
            }
        }
        json["dependencies"] = materials_json;
    }

    void Model::Deserialize(const nlohmann::json& json)
    {
        m_Materials.clear();

        if (!json.contains("dependencies")) {
            return;
        }

        const auto& dependencies = json["dependencies"];
        if (!dependencies.is_object()) {
            return;
        }

        u32 max_index = 0;
        std::unordered_map<u32, UUID> temp_map;

        for (const auto& [key_str, value] : dependencies.items()) {
            try {
                u32 index = static_cast<u32>(std::stoul(key_str));
                UUID uuid = UUID::FromString(value.get<std::string>());
                temp_map[index] = uuid;
                max_index = std::max(max_index, index);
            }
            catch (...) {}
        }

        m_Materials.resize(max_index + 1);
        for (const auto& [index, uuid] : temp_map) {
            if (index < m_Materials.size()) {
                m_Materials[index] = uuid;
            }
        }
    }
}
