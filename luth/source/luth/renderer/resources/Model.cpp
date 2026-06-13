#include "luthpch.h"
#include "luth/renderer/resources/Model.h"
#include "luth/renderer/Renderer.h"
#include "luth/core/diagnostics/Log.h"
#include "luth/renderer/backend/vulkan/VulkanBuffer.h"
#include "luth/renderer/backend/vulkan/VulkanAccelerationStructure.h"
#include "luth/resources/AssetManager.h"

namespace Luth
{
    std::shared_ptr<Model> Model::Create(const std::vector<MeshData>& meshData, const std::vector<UUID>& materials)
    {
        auto model = std::make_shared<Model>();
        model->m_MeshesData = meshData;
        model->m_Materials = materials;
        model->ProcessMeshData();
        return model;
    }

    std::shared_ptr<Model> Model::Create(const std::vector<MeshData>& meshData, const std::vector<UUID>& materials,
        const Skeleton& skeleton, const std::vector<UUID>& clipUUIDs, bool isSkinned)
    {
        auto model = std::make_shared<Model>();
        model->m_MeshesData = meshData;
        model->m_Materials = materials;
        model->m_Skeleton = skeleton;
        model->m_AnimationClipUUIDs = clipUUIDs;
        model->m_IsSkinned = isSkinned;
        model->ProcessMeshData();
        return model;
    }

    void Model::ProcessMeshData()
    {
        // Upload to GPU (Main Thread)
        for (const auto& data : m_MeshesData)
        {
            std::shared_ptr<VertexBuffer> vb;

            if (data.IsSkinned && !data.SkinnedVertices.empty())
            {
                vb = VertexBuffer::Create(data.SkinnedVertices.data(),
                    data.SkinnedVertices.size() * sizeof(SkinnedVertex));

                vb->SetLayout({
                    { ShaderDataType::Float3, "a_Position"    },
                    { ShaderDataType::Float3, "a_Normal"      },
                    { ShaderDataType::Float2, "a_TexCoord0"   },
                    { ShaderDataType::Float2, "a_TexCoord1"   },
                    { ShaderDataType::Float3, "a_Tangent"     },
                    { ShaderDataType::Int4,   "a_BoneIDs"     },
                    { ShaderDataType::Float4, "a_BoneWeights" }
                });
            }
            else
            {
                vb = VertexBuffer::Create(data.Vertices.data(),
                    data.Vertices.size() * sizeof(Vertex));

                vb->SetLayout({
                    { ShaderDataType::Float3, "a_Position"  },
                    { ShaderDataType::Float3, "a_Normal"    },
                    { ShaderDataType::Float2, "a_TexCoord0" },
                    { ShaderDataType::Float2, "a_TexCoord1" },
                    { ShaderDataType::Float3, "a_Tangent"   }
                });
            }

            auto ib = IndexBuffer::Create(data.Indices.data(), data.Indices.size());
            const u32 vertCount = data.IsSkinned
                ? static_cast<u32>(data.SkinnedVertices.size())
                : static_cast<u32>(data.Vertices.size());
            auto mesh = Mesh::Create(vb, ib, vertCount, data.IsSkinned);

            // Per-mesh BLAS — static built once over the source VB; skinned built over the deformed
            // positions buffer (filled by the per-frame skinning compute) with ALLOW_UPDATE so
            // each frame's MODE_UPDATE refit is cheap. Built unconditionally on Vulkan backends;
            // factory returns null on non-Vulkan backends (graceful — TLAS skips meshes with no BLAS).
            if (data.IsSkinned)
                mesh->SetBlas(VKAccelerationStructure::CreateSkinnedBLAS(*mesh, data.SkinnedVertices));
            else
                mesh->SetBlas(VKAccelerationStructure::CreateStaticBLAS(*mesh));

            m_Meshes.push_back(mesh);
        }

        CacheModelInfo();
    }

    std::vector<Mat4> Model::ComputeNodeWorldTransforms() const
    {
        std::vector<Mat4> world(m_Nodes.size());
        for (size_t i = 0; i < m_Nodes.size(); ++i) {
            const auto& n = m_Nodes[i];
            Mat4 local = Math::Translate(Mat4(1.0f), n.Translation)
                       * Math::ToMat4(n.Rotation)
                       * Math::Scale(Mat4(1.0f), n.Scale);
            // Nodes are topological — a parent's world matrix is finalized before its children read it.
            world[i] = (n.ParentIndex >= 0) ? world[n.ParentIndex] * local : local;
        }
        return world;
    }

    ModelInfo Model::GetModelInfo() const
    {
        ModelInfo info;
        info.IsSkinned = m_IsSkinned;
        info.TotalMeshCount = (u32)m_Meshes.size();
        info.MaterialCount = (u32)m_Materials.size();

        // Calculate totals
        for (const auto& meshData : m_MeshesData) {
            u32 vertCount = meshData.IsSkinned
                ? static_cast<u32>(meshData.SkinnedVertices.size())
                : static_cast<u32>(meshData.Vertices.size());

            info.TotalVertexCount += vertCount;
            info.TotalIndexCount += static_cast<u32>(meshData.Indices.size());

            MeshInfo meshInfo;
            meshInfo.Name = meshData.Name;
            meshInfo.VertexCount = vertCount;
            meshInfo.IndexCount = static_cast<u32>(meshData.Indices.size());
            meshInfo.MaterialIndex = meshData.MaterialIndex;
            info.Meshes.push_back(meshInfo);
        }

        // Populate skeleton info
        info.BoneCount = m_Skeleton.BoneCount();
        info.AnimationCount = static_cast<u32>(m_AnimationClipUUIDs.size());

        for (const auto& bone : m_Skeleton.Bones) {
            BoneNodeInfo bni;
            bni.Name = bone.Name;
            bni.ParentIndex = bone.ParentIndex;
            bni.BoneIndex = static_cast<int>(info.BoneHierarchy.size());
            info.BoneHierarchy.push_back(bni);
        }

        // Best-effort clip metadata. Cached on first GetModelInfo() call (during
        // ProcessMeshData), so async-loaded clips show as "<not loaded>" until
        // CacheModelInfo() runs again.
        for (const auto& uuid : m_AnimationClipUUIDs) {
            AnimationInfo ai;
            if (auto clip = AssetManager::GetAsset<AnimationClip>(uuid)) {
                ai.Name = clip->Name;
                ai.Duration = static_cast<double>(clip->Duration);
                ai.TicksPerSecond = static_cast<double>(clip->TicksPerSecond);
            }
            else {
                ai.Name = "<not loaded>";
            }
            info.Animations.push_back(ai);
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
