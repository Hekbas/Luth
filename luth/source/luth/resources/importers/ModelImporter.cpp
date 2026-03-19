#include "luthpch.h"
#include "ModelImporter.h"
#include "luth/core/Math.h"
#include "luth/resources/AssetDatabase.h"
#include "luth/resources/MetaFile.h"
#include "luth/resources/FileSystem.h"
#include "luth/resources/AssetSerializer.h"
#include "luth/renderer/Material.h"

#include <assimp/Importer.hpp>
#include <assimp/scene.h>
#include <assimp/postprocess.h>
#include <assimp/GltfMaterial.h>
#include <fstream>

namespace Luth
{
    // --- Helpers ---
    static Mat4 AxisCorrectionMatrix(const aiScene* scene)
    {
        Mat4 correction = Mat4(1.0f);

        if (scene->mMetaData) {
            int upAxis = 1, frontAxis = 1;
            int upSign = 1, frontSign = 1;
            bool hasUp = false, hasFront = false;

            // Extract axis information from metadata
            hasUp = scene->mMetaData->Get("UpAxis", upAxis);
            hasFront = scene->mMetaData->Get("FrontAxis", frontAxis);

            // Handle sign
            if (hasUp) { upSign = upAxis >= 0 ? 1 : -1; upAxis = abs(upAxis); }
            if (hasFront) { frontSign = frontAxis >= 0 ? 1 : -1; frontAxis = abs(frontAxis); }

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
        return correction;
    }

    static MeshData ProcessMesh(aiMesh* mesh, const aiScene* scene, const Mat4& transform)
    {
        MeshData data;
        data.Name = mesh->mName.C_Str();
        data.MaterialIndex = mesh->mMaterialIndex;

        // Vertices
        data.Vertices.reserve(mesh->mNumVertices);
        for (unsigned int i = 0; i < mesh->mNumVertices; i++) {
            Vertex vertex;
            
            // Position
            Vec4 pos = transform * Vec4(AiVec3ToGLM(mesh->mVertices[i]), 1.0f);
            vertex.Position = Vec3(pos);

            // Normal (Transform with Normal Matrix)
            Mat3 normalMatrix = ConvertToNormalMatrix(transform);
            if (mesh->HasNormals())
                vertex.Normal = glm::normalize(normalMatrix * AiVec3ToGLM(mesh->mNormals[i]));
            else
                vertex.Normal = Vec3(0.0f);

            // Texture Coordinates
            if (mesh->mTextureCoords[0]) {
                vertex.TexCoord0 = Vec2(mesh->mTextureCoords[0][i].x, mesh->mTextureCoords[0][i].y);
            } else {
                vertex.TexCoord0 = Vec2(0.0f);
            }

            // Tangents
            if (mesh->HasTangentsAndBitangents()) {
                vertex.Tangent = glm::normalize(normalMatrix * AiVec3ToGLM(mesh->mTangents[i]));
            } else {
                vertex.Tangent = Vec3(0.0f);
            }

            data.Vertices.push_back(vertex);
        }

        // Indices
        for (unsigned int i = 0; i < mesh->mNumFaces; i++) {
            aiFace face = mesh->mFaces[i];
            for (unsigned int j = 0; j < face.mNumIndices; j++)
                data.Indices.push_back(face.mIndices[j]);
        }

        return data;
    }

    static void ProcessNode(aiNode* node, const aiScene* scene, const Mat4& parentTransform, std::vector<MeshData>& outMeshes)
    {
        Mat4 transform = parentTransform * AiMat4ToGLM(node->mTransformation);

        for (unsigned int i = 0; i < node->mNumMeshes; i++) {
            aiMesh* mesh = scene->mMeshes[node->mMeshes[i]];
            outMeshes.push_back(ProcessMesh(mesh, scene, transform));
        }

        for (unsigned int i = 0; i < node->mNumChildren; i++) {
            ProcessNode(node->mChildren[i], scene, transform, outMeshes);
        }
    }

    static MapType AssimpToLuthMapType(aiTextureType type)
    {
        switch (type) {
            case aiTextureType_DIFFUSE:             return MapType::Diffuse;
            case aiTextureType_BASE_COLOR:          return MapType::Diffuse;
            case aiTextureType_NORMALS:             return MapType::Normal;
            case aiTextureType_NORMAL_CAMERA:       return MapType::Normal;
            case aiTextureType_METALNESS:           return MapType::Metalness;
            case aiTextureType_DIFFUSE_ROUGHNESS:   return MapType::Roughness;
            case aiTextureType_EMISSIVE:            return MapType::Emissive;
            case aiTextureType_AMBIENT:             return MapType::Occlusion;
            default:                                return MapType::Diffuse;
        }
    }

    // --- Importer Logic ---

    struct ImportContext {
        fs::path SourcePath;
        fs::path TextureDir;
        fs::path MaterialDir;
        const aiScene* Scene;
        std::unordered_map<std::string, UUID> TexturePathToUUID;
        std::vector<UUID> MaterialUUIDs;
    };

    static void ProcessTextures(ImportContext& ctx)
    {
        if (!ctx.Scene->HasTextures() && !ctx.Scene->HasMaterials()) return;

        // Ensure texture directory exists
        if (!fs::exists(ctx.TextureDir))
            fs::create_directories(ctx.TextureDir);

        // 1. Extract Embedded Textures
        for (unsigned int i = 0; i < ctx.Scene->mNumTextures; ++i)
        {
            aiTexture* texture = ctx.Scene->mTextures[i];
            std::string fileName = ctx.SourcePath.stem().string() + "_Tex_" + std::to_string(i) + ".png";
            
            // If hint is available, use it for extension
            if (texture->achFormatHint[0] != 0) {
                fileName = ctx.SourcePath.stem().string() + "_Tex_" + std::to_string(i) + "." + std::string(texture->achFormatHint);
            }

            fs::path destPath = ctx.TextureDir / fileName;

            // Write to disk if not exists
            if (!fs::exists(destPath)) {
                if (texture->mHeight == 0) {
                    // Compressed format (jpg/png), write directly
                    std::ofstream file(destPath, std::ios::binary);
                    file.write((const char*)texture->pcData, texture->mWidth);
                } else {
                    // Raw ARGB8888, would need encoding (skip for now or use stbi_write)
                    LH_CORE_WARN("Raw embedded textures not fully supported yet: {0}", fileName);
                    continue;
                }
            }

            // Register
            UUID uuid = AssetDatabase::GetUUID(destPath);
            if (!uuid.IsValid()) uuid = MetaFile::Create(destPath, AssetType::Texture);
            AssetDatabase::RegisterAsset(destPath, uuid, AssetType::Texture);
            
            // Map "*0", "*1" to this UUID
            ctx.TexturePathToUUID["*" + std::to_string(i)] = uuid;
        }
    }

    static UUID ProcessMaterial(ImportContext& ctx, aiMaterial* aiMat, int index)
    {
        aiString name;
        aiMat->Get(AI_MATKEY_NAME, name);
        std::string matName = name.C_Str();
        if (matName.empty()) matName = "Material_" + std::to_string(index);

        // Sanitize name
        std::replace(matName.begin(), matName.end(), ':', '_');
        std::replace(matName.begin(), matName.end(), '/', '_');
        std::replace(matName.begin(), matName.end(), '\\', '_');

        fs::path matPath = ctx.MaterialDir / (matName + ".mat");

        // Check if material already exists
        UUID matUUID = AssetDatabase::GetUUID(matPath);
        if (matUUID.IsValid()) return matUUID;

        // Create new Material
        nlohmann::json matJson;
        UUID pbrUUID = AssetDatabase::GetUUID(FileSystem::AssetsPath() / "shaders/pbr.vert");
        matJson["shader"] = pbrUUID.IsValid() ? pbrUUID.ToString() : "";
        matJson["render_mode"] = 0; // Opaque

        // Properties
        nlohmann::json uniforms;
        aiColor4D color;
        if (aiMat->Get(AI_MATKEY_BASE_COLOR, color) == AI_SUCCESS) {
            uniforms["u_AlbedoColor"] = { color.r, color.g, color.b, color.a };
        } else if (aiMat->Get(AI_MATKEY_COLOR_DIFFUSE, color) == AI_SUCCESS) {
            uniforms["u_AlbedoColor"] = { color.r, color.g, color.b, color.a };
        }

        float floatVal;
        if (aiMat->Get(AI_MATKEY_METALLIC_FACTOR, floatVal) == AI_SUCCESS) uniforms["u_Metalness"] = floatVal;
        if (aiMat->Get(AI_MATKEY_ROUGHNESS_FACTOR, floatVal) == AI_SUCCESS) uniforms["u_Roughness"] = floatVal;
        matJson["uniforms"] = uniforms;

        // Textures
        matJson["textures"] = nlohmann::json::array();
        
        auto TryAddTexture = [&](aiTextureType aiType, MapType luthType) {
            if (aiMat->GetTextureCount(aiType) > 0) {
                aiString path;
                if (aiMat->GetTexture(aiType, 0, &path) == AI_SUCCESS) {
                    std::string pathStr = path.C_Str();
                    UUID texUUID = UUID::Invalid();

                    // Check embedded map
                    if (ctx.TexturePathToUUID.count(pathStr)) {
                        texUUID = ctx.TexturePathToUUID[pathStr];
                    }
                    // Check external file
                    else {
                        fs::path texPath = ctx.SourcePath.parent_path() / pathStr;
                        if (fs::exists(texPath)) {
                            texUUID = AssetDatabase::GetUUID(texPath);
                            if (!texUUID.IsValid()) {
                                // Auto-import external texture
                                texUUID = MetaFile::Create(texPath, AssetType::Texture);
                                AssetDatabase::RegisterAsset(texPath, texUUID, AssetType::Texture);
                            }
                        }
                    }

                    if (texUUID.IsValid()) {
                        nlohmann::json texNode;
                        texNode["type"] = (int)luthType;
                        texNode["uuid"] = texUUID.ToString();
                        texNode["uv"] = 0;
                        texNode["useTexture"] = true;
                        matJson["textures"].push_back(texNode);
                    }
                }
            }
        };

        TryAddTexture(aiTextureType_DIFFUSE, MapType::Diffuse);
        TryAddTexture(aiTextureType_BASE_COLOR, MapType::Diffuse);
        TryAddTexture(aiTextureType_NORMALS, MapType::Normal);
        TryAddTexture(aiTextureType_METALNESS, MapType::Metalness);
        TryAddTexture(aiTextureType_DIFFUSE_ROUGHNESS, MapType::Roughness);
        TryAddTexture(aiTextureType_EMISSIVE, MapType::Emissive);

        // Save Material
        if (!fs::exists(ctx.MaterialDir)) fs::create_directories(ctx.MaterialDir);
        
        std::ofstream file(matPath);
        file << matJson.dump(4);
        file.close();

        // Register
        matUUID = MetaFile::Create(matPath, AssetType::Material);
        AssetDatabase::RegisterAsset(matPath, matUUID, AssetType::Material);
        
        return matUUID;
    }

    bool ModelImporter::Import(const std::filesystem::path& source, const std::filesystem::path& destination)
    {
        LH_PROFILE_FUNCTION();

        // Setup Context
        ImportContext ctx;
        ctx.SourcePath = source;
        ctx.TextureDir = source.parent_path() / (source.stem().string() + "_Textures");
        ctx.MaterialDir = source.parent_path() / (source.stem().string() + "_Materials");

        Assimp::Importer importer;
        // Important: Read file with flags to generate what we need
        const aiScene* scene = importer.ReadFile(source.string(),
            aiProcess_Triangulate | aiProcess_GenSmoothNormals | aiProcess_FlipUVs |
            aiProcess_CalcTangentSpace | aiProcess_JoinIdenticalVertices);

        if (!scene || scene->mFlags & AI_SCENE_FLAGS_INCOMPLETE || !scene->mRootNode) {
            LH_CORE_ERROR("ModelImporter: Failed to load model {0} : {1}", source.string(), importer.GetErrorString());
            return false;
        }

        ctx.Scene = scene;

        // 1. Process Textures
        ProcessTextures(ctx);

        // 2. Process Materials
        ctx.MaterialUUIDs.resize(scene->mNumMaterials);
        for (unsigned int i = 0; i < scene->mNumMaterials; i++) {
            ctx.MaterialUUIDs[i] = ProcessMaterial(ctx, scene->mMaterials[i], i);
        }

        // 3. Process Geometry
        ModelAssetData modelData;
        ProcessNode(scene->mRootNode, scene, AxisCorrectionMatrix(scene), modelData.Meshes);
        modelData.Materials = ctx.MaterialUUIDs;

        return AssetSerializer::SerializeModel(destination, modelData);
    }
}