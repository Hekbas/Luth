#include "luthpch.h"
#include "ModelImporter.h"
#include "luth/core/Math.h"
#include "luth/resources/AssetDatabase.h"
#include "luth/resources/MetaFile.h"
#include "luth/resources/FileSystem.h"
#include "luth/resources/AssetSerializer.h"
#include "luth/renderer/Material.h"
#include "luth/renderer/Skeleton.h"
#include "luth/renderer/AnimationClip.h"

#include <assimp/Importer.hpp>
#include <assimp/scene.h>
#include <assimp/postprocess.h>
#include <assimp/GltfMaterial.h>
#include <fstream>
#include <queue>

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

            hasUp = scene->mMetaData->Get("UpAxis", upAxis);
            hasFront = scene->mMetaData->Get("FrontAxis", frontAxis);

            if (hasUp) { upSign = upAxis >= 0 ? 1 : -1; upAxis = abs(upAxis); }
            if (hasFront) { frontSign = frontAxis >= 0 ? 1 : -1; frontAxis = abs(frontAxis); }

            if (hasUp && upAxis == 2) {
                correction = glm::rotate(correction, glm::radians(-90.0f), Vec3(1.0f, 0.0f, 0.0f));
                if (hasFront && frontAxis == 1) {
                    correction = glm::rotate(correction, glm::radians(90.0f), Vec3(0.0f, 0.0f, 1.0f));
                }
            }

            if (scene->mMetaData->HasKey("AxisMode")) {
                int axisMode;
                if (scene->mMetaData->Get("AxisMode", axisMode)) {
                    if (axisMode == 2) {
                        correction = glm::scale(correction, Vec3(-1.0f, 1.0f, 1.0f));
                    }
                }
            }
        }
        return correction;
    }

    // --- Skeleton Extraction ---

    static bool SceneHasBones(const aiScene* scene)
    {
        for (unsigned int i = 0; i < scene->mNumMeshes; ++i) {
            if (scene->mMeshes[i]->HasBones())
                return true;
        }
        return false;
    }

    // Collect all bone names and inverse bind poses from all meshes
    static void CollectBoneData(const aiScene* scene,
        std::unordered_map<std::string, Mat4>& boneInvBindPoses)
    {
        for (unsigned int mi = 0; mi < scene->mNumMeshes; ++mi) {
            aiMesh* mesh = scene->mMeshes[mi];
            if (!mesh->HasBones()) continue;

            for (unsigned int bi = 0; bi < mesh->mNumBones; ++bi) {
                aiBone* bone = mesh->mBones[bi];
                std::string name = bone->mName.C_Str();
                if (boneInvBindPoses.find(name) == boneInvBindPoses.end()) {
                    boneInvBindPoses[name] = AiMat4ToGLM(bone->mOffsetMatrix);
                }
            }
        }
    }

    // BFS from root to build skeleton in topological order
    // Include nodes that are bones OR ancestors of bones
    static void ExtractSkeleton(const aiScene* scene, Skeleton& skeleton, const Mat4& axisCorrection)
    {
        std::unordered_map<std::string, Mat4> boneInvBindPoses;
        CollectBoneData(scene, boneInvBindPoses);

        if (boneInvBindPoses.empty()) return;

        // First pass: mark all nodes that are bones or ancestors of bones
        std::unordered_set<const aiNode*> relevantNodes;

        // Find all bone nodes and mark their ancestor chains
        std::function<const aiNode*(const aiNode*, const std::string&)> FindNode;
        FindNode = [&](const aiNode* node, const std::string& name) -> const aiNode* {
            if (std::string(node->mName.C_Str()) == name) return node;
            for (unsigned int i = 0; i < node->mNumChildren; ++i) {
                auto result = FindNode(node->mChildren[i], name);
                if (result) return result;
            }
            return nullptr;
        };

        for (const auto& [boneName, _] : boneInvBindPoses) {
            const aiNode* boneNode = FindNode(scene->mRootNode, boneName);
            if (!boneNode) continue;

            // Walk up to root, marking all ancestors
            const aiNode* current = boneNode;
            while (current) {
                if (relevantNodes.count(current)) break; // Already marked
                relevantNodes.insert(current);
                current = current->mParent;
            }
        }

        // BFS to build skeleton in topological order (parents before children)
        struct QueueEntry {
            const aiNode* node;
            i32 parentIndex;
        };

        std::queue<QueueEntry> bfsQueue;
        bfsQueue.push({ scene->mRootNode, -1 });

        while (!bfsQueue.empty()) {
            auto [node, parentIdx] = bfsQueue.front();
            bfsQueue.pop();

            if (!relevantNodes.count(node)) continue;

            std::string name = node->mName.C_Str();

            BoneInfo info;
            info.Name = name;
            info.ParentIndex = parentIdx;
            info.LocalBindPose = AiMat4ToGLM(node->mTransformation);

            // Apply axis correction to root node's local bind pose
            if (parentIdx == -1) {
                info.LocalBindPose = axisCorrection * info.LocalBindPose;
            }

            auto it = boneInvBindPoses.find(name);
            if (it != boneInvBindPoses.end()) {
                info.InverseBindPose = it->second;
            }
            // else: structural node (armature), keep identity inverse bind pose

            i32 currentIndex = static_cast<i32>(skeleton.Bones.size());
            skeleton.BoneNameToIndex[name] = currentIndex;
            skeleton.Bones.push_back(info);

            // Enqueue children
            for (unsigned int i = 0; i < node->mNumChildren; ++i) {
                if (relevantNodes.count(node->mChildren[i])) {
                    bfsQueue.push({ node->mChildren[i], currentIndex });
                }
            }
        }

        LH_CORE_INFO("ModelImporter: Extracted skeleton with {0} bones ({1} actual bones, rest structural)",
            skeleton.BoneCount(), boneInvBindPoses.size());
    }

    // --- Bone Weight Extraction ---

    static void ExtractBoneWeights(aiMesh* mesh, MeshData& data, const Skeleton& skeleton)
    {
        u32 vertexCount = static_cast<u32>(data.SkinnedVertices.size());

        // Temporary per-vertex bone influence counters
        std::vector<u32> influenceCount(vertexCount, 0);

        for (unsigned int bi = 0; bi < mesh->mNumBones; ++bi) {
            aiBone* bone = mesh->mBones[bi];
            std::string boneName = bone->mName.C_Str();
            i32 boneIndex = skeleton.FindBone(boneName);

            if (boneIndex < 0) {
                LH_CORE_WARN("ModelImporter: Bone '{0}' not found in skeleton", boneName);
                continue;
            }

            for (unsigned int wi = 0; wi < bone->mNumWeights; ++wi) {
                u32 vertexId = bone->mWeights[wi].mVertexId;
                f32 weight = bone->mWeights[wi].mWeight;

                if (vertexId >= vertexCount || weight <= 0.0f) continue;

                u32& count = influenceCount[vertexId];
                if (count < MAX_BONES_PER_VERTEX) {
                    data.SkinnedVertices[vertexId].BoneIDs[count] = boneIndex;
                    data.SkinnedVertices[vertexId].BoneWeights[count] = weight;
                    count++;
                }
            }
        }

        // Normalize weights and fix vertices with no influences
        for (u32 i = 0; i < vertexCount; ++i) {
            auto& sv = data.SkinnedVertices[i];

            if (influenceCount[i] == 0) {
                // No bone influences — bind to first bone with full weight
                sv.BoneIDs = glm::ivec4(0, 0, 0, 0);
                sv.BoneWeights = Vec4(1.0f, 0.0f, 0.0f, 0.0f);
                continue;
            }

            // Normalize weights to sum to 1.0
            f32 total = sv.BoneWeights.x + sv.BoneWeights.y + sv.BoneWeights.z + sv.BoneWeights.w;
            if (total > 0.0f && std::abs(total - 1.0f) > 0.001f) {
                sv.BoneWeights /= total;
            }
        }
    }

    // --- Animation Clip Extraction ---

    static void ExtractAnimationClips(const aiScene* scene, const Skeleton& skeleton,
        std::vector<AnimationClip>& clips)
    {
        for (unsigned int ai = 0; ai < scene->mNumAnimations; ++ai) {
            aiAnimation* anim = scene->mAnimations[ai];

            AnimationClip clip;
            clip.Name = anim->mName.C_Str();
            if (clip.Name.empty()) clip.Name = "Animation_" + std::to_string(ai);
            clip.Duration = static_cast<f32>(anim->mDuration);
            clip.TicksPerSecond = static_cast<f32>(anim->mTicksPerSecond);
            if (clip.TicksPerSecond <= 0.0f) clip.TicksPerSecond = 25.0f;

            for (unsigned int ci = 0; ci < anim->mNumChannels; ++ci) {
                aiNodeAnim* channel = anim->mChannels[ci];
                std::string nodeName = channel->mNodeName.C_Str();

                i32 boneIndex = skeleton.FindBone(nodeName);
                if (boneIndex < 0) continue; // Not a bone we track

                BoneTrack track;
                track.BoneIndex = boneIndex;

                // Position keyframes
                track.Positions.reserve(channel->mNumPositionKeys);
                for (unsigned int k = 0; k < channel->mNumPositionKeys; ++k) {
                    VectorKey key;
                    key.Time = static_cast<f32>(channel->mPositionKeys[k].mTime);
                    key.Value = AiVec3ToGLM(channel->mPositionKeys[k].mValue);
                    track.Positions.push_back(key);
                }

                // Rotation keyframes
                track.Rotations.reserve(channel->mNumRotationKeys);
                for (unsigned int k = 0; k < channel->mNumRotationKeys; ++k) {
                    QuatKey key;
                    key.Time = static_cast<f32>(channel->mRotationKeys[k].mTime);
                    key.Value = AiQuatToGLM(channel->mRotationKeys[k].mValue);
                    track.Rotations.push_back(key);
                }

                // Scale keyframes
                track.Scales.reserve(channel->mNumScalingKeys);
                for (unsigned int k = 0; k < channel->mNumScalingKeys; ++k) {
                    VectorKey key;
                    key.Time = static_cast<f32>(channel->mScalingKeys[k].mTime);
                    key.Value = AiVec3ToGLM(channel->mScalingKeys[k].mValue);
                    track.Scales.push_back(key);
                }

                clip.Tracks.push_back(std::move(track));

                // Detect root motion: root bone (index 0 or 1) has translation keyframes
                if (boneIndex <= 1 && channel->mNumPositionKeys > 1) {
                    clip.HasRootMotion = true;
                }
            }

            clips.push_back(std::move(clip));
        }

        LH_CORE_INFO("ModelImporter: Extracted {0} animation clips", clips.size());
    }

    // --- Mesh Processing ---

    static MeshData ProcessStaticMesh(aiMesh* mesh, const aiScene* scene, const Mat4& transform)
    {
        MeshData data;
        data.Name = mesh->mName.C_Str();
        data.MaterialIndex = mesh->mMaterialIndex;
        data.IsSkinned = false;

        data.Vertices.reserve(mesh->mNumVertices);
        for (unsigned int i = 0; i < mesh->mNumVertices; i++) {
            Vertex vertex;

            Vec4 pos = transform * Vec4(AiVec3ToGLM(mesh->mVertices[i]), 1.0f);
            vertex.Position = Vec3(pos);

            Mat3 normalMatrix = ConvertToNormalMatrix(transform);
            if (mesh->HasNormals())
                vertex.Normal = glm::normalize(normalMatrix * AiVec3ToGLM(mesh->mNormals[i]));
            else
                vertex.Normal = Vec3(0.0f);

            if (mesh->mTextureCoords[0])
                vertex.TexCoord0 = Vec2(mesh->mTextureCoords[0][i].x, mesh->mTextureCoords[0][i].y);
            else
                vertex.TexCoord0 = Vec2(0.0f);

            if (mesh->HasTangentsAndBitangents())
                vertex.Tangent = glm::normalize(normalMatrix * AiVec3ToGLM(mesh->mTangents[i]));
            else
                vertex.Tangent = Vec3(0.0f);

            data.Vertices.push_back(vertex);
        }

        for (unsigned int i = 0; i < mesh->mNumFaces; i++) {
            aiFace face = mesh->mFaces[i];
            for (unsigned int j = 0; j < face.mNumIndices; j++)
                data.Indices.push_back(face.mIndices[j]);
        }

        return data;
    }

    // For skinned meshes: do NOT bake node transforms into vertices (skeleton handles that)
    static MeshData ProcessSkinnedMesh(aiMesh* mesh, const aiScene* scene, const Skeleton& skeleton)
    {
        MeshData data;
        data.Name = mesh->mName.C_Str();
        data.MaterialIndex = mesh->mMaterialIndex;
        data.IsSkinned = true;

        data.SkinnedVertices.reserve(mesh->mNumVertices);
        for (unsigned int i = 0; i < mesh->mNumVertices; i++) {
            SkinnedVertex vertex;

            // No transform baking — positions stay in mesh-local space
            vertex.Position = AiVec3ToGLM(mesh->mVertices[i]);

            if (mesh->HasNormals())
                vertex.Normal = AiVec3ToGLM(mesh->mNormals[i]);
            else
                vertex.Normal = Vec3(0.0f);

            if (mesh->mTextureCoords[0])
                vertex.TexCoord0 = Vec2(mesh->mTextureCoords[0][i].x, mesh->mTextureCoords[0][i].y);
            else
                vertex.TexCoord0 = Vec2(0.0f);

            if (mesh->HasTangentsAndBitangents())
                vertex.Tangent = AiVec3ToGLM(mesh->mTangents[i]);
            else
                vertex.Tangent = Vec3(0.0f);

            // BoneIDs and BoneWeights initialized to defaults by SkinnedVertex constructor
            data.SkinnedVertices.push_back(vertex);
        }

        // Indices
        for (unsigned int i = 0; i < mesh->mNumFaces; i++) {
            aiFace face = mesh->mFaces[i];
            for (unsigned int j = 0; j < face.mNumIndices; j++)
                data.Indices.push_back(face.mIndices[j]);
        }

        // Extract bone weights
        if (mesh->HasBones()) {
            ExtractBoneWeights(mesh, data, skeleton);
        }

        return data;
    }

    static void ProcessNode(aiNode* node, const aiScene* scene, const Mat4& parentTransform,
        std::vector<MeshData>& outMeshes, bool isSkinned, const Skeleton& skeleton)
    {
        Mat4 transform = parentTransform * AiMat4ToGLM(node->mTransformation);

        for (unsigned int i = 0; i < node->mNumMeshes; i++) {
            aiMesh* mesh = scene->mMeshes[node->mMeshes[i]];

            if (isSkinned && mesh->HasBones()) {
                outMeshes.push_back(ProcessSkinnedMesh(mesh, scene, skeleton));
            } else {
                outMeshes.push_back(ProcessStaticMesh(mesh, scene, transform));
            }
        }

        for (unsigned int i = 0; i < node->mNumChildren; i++) {
            ProcessNode(node->mChildren[i], scene, transform, outMeshes, isSkinned, skeleton);
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

        u32 flags = aiProcess_Triangulate | aiProcess_GenSmoothNormals | aiProcess_FlipUVs |
            aiProcess_CalcTangentSpace | aiProcess_JoinIdenticalVertices |
            aiProcess_LimitBoneWeights; // Limit to 4 bones per vertex

        const aiScene* scene = importer.ReadFile(source.string(), flags);

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

        // 3. Extract Skeleton (if model has bones)
        ModelAssetData modelData;
        Mat4 axisCorrection = AxisCorrectionMatrix(scene);
        bool isSkinned = SceneHasBones(scene);
        modelData.IsSkinned = isSkinned;

        if (isSkinned) {
            ExtractSkeleton(scene, modelData.SkeletonData, axisCorrection);
            ExtractAnimationClips(scene, modelData.SkeletonData, modelData.AnimationClips);
        }

        // 4. Process Geometry
        ProcessNode(scene->mRootNode, scene, axisCorrection, modelData.Meshes, isSkinned, modelData.SkeletonData);
        modelData.Materials = ctx.MaterialUUIDs;

        return AssetSerializer::SerializeModel(destination, modelData);
    }
}