#include "luthpch.h"
#include "luth/renderer/Model.h"
#include "luth/resources/Resources.h"

#include <assimp/Importer.hpp>
#include <assimp/postprocess.h>
#include <string>

namespace Luth
{
    Model::Model(const fs::path& path)
    {
        LoadModel(path);
    }

    void Model::Init()
    {
        ProcessMeshData();

        // Deserialize materials
        fs::path metaPath = m_Path;
        metaPath += ".meta";

        if (fs::exists(metaPath)) {
            std::ifstream file(metaPath);
            if (file.good()) {
                nlohmann::json json;
                file >> json;
                Deserialize(json);
            }
        }
        else {
            LH_CORE_WARN("No meta file found for model: {0}", m_Path.string());
        }

        CacheModelInfo();
    }

    void Model::Serialize(nlohmann::json& json) const
    {
        nlohmann::json materials_json;
        for (size_t i = 0; i < m_Materials.size(); ++i) {
            if (m_Materials[i]) {
                materials_json[std::to_string(i)] = m_Materials[i].ToString();
            }
        }
        json["dependencies"] = materials_json;
    }

    void Model::Deserialize(const nlohmann::json& json)
    {
        m_Materials.clear();

        if (!json.contains("dependencies")) {
            LH_CORE_WARN("Missing Dependencies section");
            return;
        }

        const auto& dependencies = json["dependencies"];
        if (!dependencies.is_object()) {
            LH_CORE_WARN("Dependencies section is not an object");
            return;
        }

        u32 max_index = 0;
        std::unordered_map<u32, UUID> temp_map;

        // First pass: parse all indices and find maximum
        for (const auto& [key_str, value] : dependencies.items()) {
            try {
                u32 index = static_cast<u32>(std::stoul(key_str));

                // Handle UUID value
                UUID uuid;
                if (value.is_null()) {
                    uuid = UUID();
                }
                else if (value.is_string()) {
                    if (!UUID::FromString(value.get<std::string>(), uuid)) {
                        LH_CORE_WARN("Invalid UUID format at index {0}", index);
                        uuid = UUID();
                    }
                }
                else {
                    LH_CORE_WARN("Invalid type at index {0}", index);
                    uuid = UUID();
                }

                temp_map[index] = uuid;
                max_index = std::max(max_index, index);
            }
            catch (const std::exception& e) {
                LH_CORE_WARN("Invalid index format: {0}", key_str);
            }
        }

        // Create properly sized array
        m_Materials.resize(max_index + 1);

        // Second pass: fill the materials array
        for (const auto& [index, uuid] : temp_map) {
            if (index < m_Materials.size()) {
                m_Materials[index] = uuid;
            }
        }
    }

    void Model::LoadModel(const fs::path& path)
    {
		m_Path = path;

        f32 ti = Time::GetTime();

        Assimp::Importer importer;
        const aiScene* scene = importer.ReadFile(path.string(),
            aiProcess_Triangulate | aiProcess_GenSmoothNormals | aiProcess_FlipUVs |
            aiProcess_CalcTangentSpace | aiProcess_JoinIdenticalVertices);

        if (!scene || scene->mFlags & AI_SCENE_FLAGS_INCOMPLETE || !scene->mRootNode) {
            LH_CORE_ERROR("{0}", importer.GetErrorString());
            return;
        }

        ProcessNode(scene->mRootNode, scene, AxisCorrectionMatrix(scene));

        // New material system :3
        LoadMaterials(path);

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
            m_MeshesData.push_back(ProcessMesh(mesh, scene, nodeTransform));
        }

        // Process children recursively
        for (uint32_t i = 0; i < node->mNumChildren; i++) {
            ProcessNode(node->mChildren[i], scene, nodeTransform);
        }
    }

    MeshData Model::ProcessMesh(aiMesh* mesh, const aiScene* scene, const Mat4& transform)
    {
        MeshData data;
        data.Name = mesh->mName.C_Str();

        data.Vertices.reserve(mesh->mNumVertices);
        data.Indices.reserve(mesh->mNumFaces * 3);  // Assuming triangulation

        const Mat3 normalMatrix = ConvertToNormalMatrix(transform);

        // Process vertices with transform
        for (uint32_t i = 0; i < mesh->mNumVertices; i++) {
            Vertex vertex{};
            const aiVector3D& pos = mesh->mVertices[i];

            const Vec4 transformedPos = transform * Vec4(pos.x, pos.y, pos.z, 1.0f);
            vertex.Position = Vec3(transformedPos);

            if (mesh->mNormals) {
                const aiVector3D& norm = mesh->mNormals[i];
                const Vec3 transformedNorm = normalMatrix * Vec3(norm.x, norm.y, norm.z);
                vertex.Normal = glm::normalize(transformedNorm);
            }

            if (mesh->mTextureCoords[0])
                vertex.TexCoord0 = { mesh->mTextureCoords[0][i].x, mesh->mTextureCoords[0][i].y };
            if (mesh->mTextureCoords[1])
                vertex.TexCoord1 = { mesh->mTextureCoords[1][i].x, mesh->mTextureCoords[1][i].y };

            if (mesh->mTangents) {
                const aiVector3D& tan = mesh->mTangents[i];
                const Vec3 transformedTangent = normalMatrix * Vec3(tan.x, tan.y, tan.z);
                vertex.Tangent = glm::normalize(transformedTangent);
            }

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

        // Process material (deprecated)
        /*if (mesh->mMaterialIndex >= 0 && static_cast<uint32_t>(mesh->mMaterialIndex) < scene->mNumMaterials) {
            data.materialIndex = static_cast<uint32_t>(m_Materials.size());
            m_Materials.push_back(ProcessMaterial(scene->mMaterials[mesh->mMaterialIndex], m_Directory));
        }*/

        return data;
    }

    Material Model::ProcessMaterial(aiMaterial* mat, const fs::path& directory)
    {
        Material material;
        //MapInfo texInfo;
        //aiString path;
        //for (uint32_t type = aiTextureType_DIFFUSE; type <= aiTextureType_UNKNOWN; type++)
        //{
        //    aiTextureType aiType = static_cast<aiTextureType>(type);
        //    for (uint32_t i = 0; i < mat->GetTextureCount(aiType); i++)
        //    {
        //        if (mat->GetTexture(aiType, i, &path, nullptr, &texInfo.uvIndex) == AI_SUCCESS) {
        //            switch (aiType) {
        //                case aiTextureType_DIFFUSE:         texInfo.type = MapType::Diffuse;   break;
        //                case aiTextureType_BASE_COLOR:      texInfo.type = MapType::Diffuse;   break;
        //                case aiTextureType_NORMALS:         texInfo.type = MapType::Normal;    break;
        //                case aiTextureType_NORMAL_CAMERA:   texInfo.type = MapType::Normal;    break;
        //                case aiTextureType_HEIGHT:          texInfo.type = MapType::Normal;    break;
        //                case aiTextureType_EMISSIVE:        texInfo.type = MapType::Emissive;  break;
        //                case aiTextureType_EMISSION_COLOR:  texInfo.type = MapType::Emissive;  break;
        //                case aiTextureType_METALNESS:       texInfo.type = MapType::Metalness; break;
        //                case aiTextureType_SHININESS:       texInfo.type = MapType::Roughness; break;
        //                default: continue; // Skip unsupported types
        //            }
        //            fs::path name = path.C_Str();
        //            auto result = Resources::Find<Model>(name.filename().stem().string() + ".*", true);
        //            if (!result.empty()) texInfo.path = result[0];
        //            material.AddTexture(texInfo);
        //        }
        //    }
        //}
        return material;
    }

    void Model::LoadMaterials(const fs::path& path)
    {
        fs::path metaPath = path;
        metaPath += ".meta";

        std::ifstream file(metaPath);
        if (!file.is_open()) {
            LH_CORE_ERROR("Failed to Load Materials. Could not find file: {0}", metaPath.string());
            return;
        }

        try {
            nlohmann::json j;
            file >> j;

            const auto& materials_json = j["materials"];
            std::unordered_map<int, UUID> temp_materials;
            int max_index = -1;

            // First pass: collect all materials and validate
            for (const auto& element : materials_json.items()) {
                int index;
                try {
                    index = std::stoi(element.key());
                }
                catch (const std::exception&) {
                    LH_CORE_ERROR("Invalid index format: {0}", element.key());
                    continue;
                }

                if (index < 0) {
                    LH_CORE_ERROR("Negative material index: {0}", std::to_string(index));
                    continue;
                }

                if (temp_materials.find(index) != temp_materials.end()) {
                    LH_CORE_ERROR("Duplicate material index: {0}", std::to_string(index));
                    continue;
                }

                std::string uuidString = element.value().get<std::string>();
                UUID uuid;
                if (!UUID::FromString(uuidString, uuid)) {
                    LH_CORE_ERROR("Error getting UUID from string: {0}", uuidString);
                    continue;
                }
                temp_materials[index] = uuid;

                if (index > max_index) {
                    max_index = index;
                }
            }

            // Create vector with appropriate size, initialized with empty strings
            std::vector<UUID> new_materials;
            if (max_index >= 0) {
                new_materials.resize(max_index + 1);
            }

            // Fill the vector with collected materials
            for (const auto& [index, uuid] : temp_materials) {
                new_materials[index] = uuid;
            }

            // Swap into member variable after successful parsing
            m_Materials.swap(new_materials);
        }
        catch (const std::exception& e) {
            LH_CORE_ERROR("Failed to parse materials JSON: {0}", std::string(e.what()));
        }
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

    void Model::ProcessMeshData()
    {
        for (auto& meshData : m_MeshesData) {
            auto vb = VertexBuffer::Create(meshData.Vertices.data(), meshData.Vertices.size() * sizeof(Vertex));
            vb->SetLayout({
                { ShaderDataType::Float3, "a_Position"  },
                { ShaderDataType::Float3, "a_Normal"    },
                { ShaderDataType::Float2, "a_TexCoord0" },
                { ShaderDataType::Float2, "a_TexCoord1" },
                { ShaderDataType::Float3, "a_Tangent"   } }
            );
            auto ib = IndexBuffer::Create(meshData.Indices.data(), meshData.Indices.size());
            m_Meshes.push_back(Mesh::Create(vb, ib));
        }
    }

    ModelInfo Model::GetModelInfo() const
    {
        ModelInfo info;
        info.Path = m_Path;
        info.IsSkinned = m_IsSkinned;
        info.TotalMeshCount = static_cast<uint32_t>(m_MeshesData.size());
        info.MaterialCount = static_cast<uint32_t>(m_Materials.size());

        // Calculate totals and per-mesh info
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
}
