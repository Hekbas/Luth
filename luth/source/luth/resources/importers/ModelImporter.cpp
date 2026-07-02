#include "luthpch.h"
#include "ModelImporter.h"
#include "luth/core/types/LuthMath.h"
#include "luth/resources/AssetDatabase.h"
#include "luth/resources/MetaFile.h"
#include "luth/resources/FileSystem.h"
#include "luth/resources/AssetSerializer.h"
#include "luth/resources/importers/TextureResolver.h"
#include "luth/resources/importers/TextureBaker.h"
#include "luth/resources/importers/ImportReport.h"
#include "luth/resources/importers/AnimationClipImporter.h"
#include "luth/renderer/material/Material.h"
#include "luth/renderer/resources/Skeleton.h"
#include "luth/renderer/resources/AnimationClip.h"

#include <assimp/Importer.hpp>
#include <assimp/scene.h>
#include <assimp/postprocess.h>
#include <assimp/GltfMaterial.h>
#include <fstream>
#include <queue>
#include <algorithm>
#include <cctype>

namespace Luth
{
    // Import report: cleared each import, readable by the editor.
    static ImportReport s_LastImportReport;

    ImportReport ModelImporter::GetLastImportReport()
    {
        return s_LastImportReport;
    }

    // ---- Import Settings Serialization ----

    ModelImportSettings ModelImportSettings::FromJson(const nlohmann::json& j)
    {
        ModelImportSettings s;
        s.ImportNormals                = j.value("import_normals", true);
        s.ImportTangents               = j.value("import_tangents", false);
        s.OptimizeMesh                 = j.value("optimize_mesh", true);
        s.MarkDeformable               = j.value("mark_deformable", false);
        s.ScaleFactor                  = j.value("scale_factor", 1.0f);
        s.UpAxis                       = j.value("up_axis", -1);
        s.BakeAxisConversion           = j.value("bake_axis_conversion", true);
        s.SkinMeshTransform            = static_cast<MeshTransformMode>(j.value("skin_mesh_transform", 0));
        s.ExtractClipsAsSeparateAssets = j.value("extract_clips_as_separate_assets", true);
        s.ImportCameras                = j.value("import_cameras", true);
        s.ImportLights                 = j.value("import_lights", true);
        s.PhysicsBake                  = static_cast<PhysicsBakeMode>(j.value("physics_bake", 0));
        s.AutoDetectTextureRoles       = j.value("auto_detect_texture_roles", true);
        return s;
    }

    nlohmann::json ModelImportSettings::ToJson() const
    {
        return {
            { "import_normals",                  ImportNormals },
            { "import_tangents",                 ImportTangents },
            { "optimize_mesh",                   OptimizeMesh },
            { "mark_deformable",                 MarkDeformable },
            { "scale_factor",                    ScaleFactor },
            { "up_axis",                         UpAxis },
            { "bake_axis_conversion",            BakeAxisConversion },
            { "skin_mesh_transform",             static_cast<int>(SkinMeshTransform) },
            { "extract_clips_as_separate_assets", ExtractClipsAsSeparateAssets },
            { "import_cameras",                  ImportCameras },
            { "import_lights",                   ImportLights },
            { "physics_bake",                    static_cast<int>(PhysicsBake) },
            { "auto_detect_texture_roles",       AutoDetectTextureRoles }
        };
    }

    // ---- Helpers ----
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
                correction = Math::Rotate(correction, Math::Radians(-90.0f), Vec3(1.0f, 0.0f, 0.0f));
                if (hasFront && frontAxis == 1) {
                    correction = Math::Rotate(correction, Math::Radians(90.0f), Vec3(0.0f, 0.0f, 1.0f));
                }
            }

            if (scene->mMetaData->HasKey("AxisMode")) {
                int axisMode;
                if (scene->mMetaData->Get("AxisMode", axisMode)) {
                    if (axisMode == 2) {
                        correction = Math::Scale(correction, Vec3(-1.0f, 1.0f, 1.0f));
                    }
                }
            }
        }
        return correction;
    }

    static bool IsNearIdentity(const Mat4& m, float tolerance = 0.01f)
    {
        Mat4 identity(1.0f);
        float diff = 0.0f;
        for (int c = 0; c < 4; c++)
            for (int r = 0; r < 4; r++)
                diff += std::abs(m[c][r] - identity[c][r]);
        return diff <= tolerance;
    }

    // Walk the scene graph from root to the first skinned mesh node, accumulating aiNode::mTransformation.
    // Captures DCC-baked rotations on mesh nodes that mOffsetMatrix (mesh-local space) doesn't account for.
    static Mat4 ComputeMeshSpaceCorrection(const aiScene* scene)
    {
        // Find the first mesh that has bones
        for (unsigned int mi = 0; mi < scene->mNumMeshes; ++mi) {
            if (!scene->mMeshes[mi]->HasBones()) continue;

            // Find which node hosts this mesh
            std::function<const aiNode*(const aiNode*, unsigned int)> FindMeshNode;
            FindMeshNode = [&](const aiNode* node, unsigned int meshIndex) -> const aiNode* {
                for (unsigned int i = 0; i < node->mNumMeshes; ++i) {
                    if (node->mMeshes[i] == meshIndex) return node;
                }
                for (unsigned int i = 0; i < node->mNumChildren; ++i) {
                    auto result = FindMeshNode(node->mChildren[i], meshIndex);
                    if (result) return result;
                }
                return nullptr;
            };

            const aiNode* meshNode = FindMeshNode(scene->mRootNode, mi);
            if (!meshNode) continue;

            // Accumulate transforms from root to mesh node (inclusive)
            std::vector<const aiNode*> chain;
            const aiNode* current = meshNode;
            while (current) {
                chain.push_back(current);
                current = current->mParent;
            }

            Mat4 accumulated(1.0f);
            for (int i = static_cast<int>(chain.size()) - 1; i >= 0; --i) {
                accumulated = accumulated * AiMat4ToGLM(chain[i]->mTransformation);
            }
            return accumulated;
        }
        return Mat4(1.0f);
    }

    // ---- Skeleton Extraction ----

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

    // BFS from root builds the skeleton in topological order, including nodes that are bones or ancestors of bones.
    // meshSpaceCorrection: accumulated root-to-skinned-mesh-node transform, lifts bone poses from mesh-local into engine space.
    static void ExtractSkeleton(const aiScene* scene, Skeleton& skeleton,
        const Mat4& axisCorrection, const Mat4& meshSpaceCorrection)
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

        auto MarkNodeAndAncestors = [&](const aiNode* node) {
            const aiNode* current = node;
            while (current) {
                if (relevantNodes.count(current)) break; // Already marked
                relevantNodes.insert(current);
                current = current->mParent;
            }
        };

        // Mark actual bone nodes and their ancestors
        for (const auto& [boneName, _] : boneInvBindPoses) {
            const aiNode* boneNode = FindNode(scene->mRootNode, boneName);
            if (!boneNode) continue;
            MarkNodeAndAncestors(boneNode);
        }

        // Mark nodes targeted by animation channels (handles $AssimpFbx$ intermediate nodes)
        for (unsigned int ai = 0; ai < scene->mNumAnimations; ++ai) {
            const aiAnimation* anim = scene->mAnimations[ai];
            for (unsigned int ci = 0; ci < anim->mNumChannels; ++ci) {
                std::string channelName = anim->mChannels[ci]->mNodeName.C_Str();
                const aiNode* channelNode = FindNode(scene->mRootNode, channelName);
                if (channelNode)
                    MarkNodeAndAncestors(channelNode);
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

            for (unsigned int i = 0; i < node->mNumChildren; ++i) {
                if (relevantNodes.count(node->mChildren[i])) {
                    bfsQueue.push({ node->mChildren[i], currentIndex });
                }
            }
        }
        
        // ---- Fix FBX Export Pose Issue ----
        // FBX stores the current timeline frame in mTransformation, NOT the T-pose. Reverse-engineer the true
        // T-pose from mOffsetMatrix to fix the "double animation" glitch.
        
        // 1. Strip the axis correction off the root temporarily to work in pure Assimp space
        for (auto& bone : skeleton.Bones) {
            if (bone.ParentIndex == -1) {
                bone.LocalBindPose = Math::Inverse(axisCorrection) * bone.LocalBindPose;
            }
        }

        // 2. Reconstruct true global poses from Assimp's Offset matrices
        std::vector<Mat4> trueGlobalBindPoses(skeleton.BoneCount(), Mat4(1.0f));
        for (u32 i = 0; i < skeleton.BoneCount(); ++i) {
            auto it = boneInvBindPoses.find(skeleton.Bones[i].Name);
            if (it != boneInvBindPoses.end()) {
                // mOffsetMatrix transforms mesh-local to bone space; its inverse is the bone global transform in
                // mesh-local space. Multiply by meshSpaceCorrection to lift into engine space (where vertices are baked).
                trueGlobalBindPoses[i] = meshSpaceCorrection * Math::Inverse(it->second);
            } else {
                // Structural node (no offset matrix): fallback to the exported pose
                i32 parent = skeleton.Bones[i].ParentIndex;
                Mat4 local = skeleton.Bones[i].LocalBindPose;
                trueGlobalBindPoses[i] = (parent >= 0) ? (trueGlobalBindPoses[parent] * local) : local;
            }
        }

        // 3. Convert true global poses back to true local poses, and re-apply axis correction to root
        for (u32 i = 0; i < skeleton.BoneCount(); ++i) {
            i32 parent = skeleton.Bones[i].ParentIndex;
            if (parent >= 0) {
                skeleton.Bones[i].LocalBindPose = Math::Inverse(trueGlobalBindPoses[parent]) * trueGlobalBindPoses[i];
            } else {
                skeleton.Bones[i].LocalBindPose = axisCorrection * trueGlobalBindPoses[i];
            }
        }

        LH_LOG(Assets, info, "ModelImporter: Extracted skeleton with {0} bones ({1} actual bones, rest structural)",
            skeleton.BoneCount(), boneInvBindPoses.size());
    }

    // Recompute InverseBindPose from the skeleton's own hierarchy. Assimp's mOffsetMatrix was computed against
    // its internal hierarchy, which may differ (e.g. $AssimpFbx$ intermediate nodes included, or axis correction
    // applied to the root). Deriving InverseBindPose from the LocalBindPose chain guarantees skin[i] = I at bind pose.
    static void RecomputeInverseBindPoses(Skeleton& skeleton)
    {
        u32 boneCount = skeleton.BoneCount();
        if (boneCount == 0) return;

        std::vector<Mat4> globalBindPose(boneCount);
        for (u32 i = 0; i < boneCount; i++)
        {
            i32 parent = skeleton.Bones[i].ParentIndex;
            globalBindPose[i] = (parent >= 0)
                ? globalBindPose[parent] * skeleton.Bones[i].LocalBindPose
                : skeleton.Bones[i].LocalBindPose;

            skeleton.Bones[i].InverseBindPose = Math::Inverse(globalBindPose[i]);
        }
    }

    // ---- Bone Weight Extraction ----

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
                LH_LOG(Assets, warn, "ModelImporter: Bone '{0}' not found in skeleton", boneName);
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
                // No bone influences: bind to first bone with full weight
                sv.BoneIDs = IVec4(0, 0, 0, 0);
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

    // ---- Animation Clip Extraction ----

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
                if (boneIndex < 0) {
                    LH_LOG(Assets, warn, "ModelImporter: Animation channel '{}' not found in skeleton — skipping", nodeName);
                    continue;
                }

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

        LH_LOG(Assets, info, "ModelImporter: Extracted {0} animation clips", clips.size());
    }

    // ---- Mesh Processing ----

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
                vertex.Normal = Math::Normalize(normalMatrix * AiVec3ToGLM(mesh->mNormals[i]));
            else
                vertex.Normal = Vec3(0.0f);

            if (mesh->mTextureCoords[0])
                vertex.TexCoord0 = Vec2(mesh->mTextureCoords[0][i].x, mesh->mTextureCoords[0][i].y);
            else
                vertex.TexCoord0 = Vec2(0.0f);

            if (mesh->mTextureCoords[1])
                vertex.TexCoord1 = Vec2(mesh->mTextureCoords[1][i].x, mesh->mTextureCoords[1][i].y);
            else
                vertex.TexCoord1 = Vec2(0.0f);

            if (mesh->HasTangentsAndBitangents())
                vertex.Tangent = Math::Normalize(normalMatrix * AiVec3ToGLM(mesh->mTangents[i]));
            else
                vertex.Tangent = Vec3(0.0f);

            data.Vertices.push_back(vertex);
        }

        // Compute bind-pose AABB from vertex positions
        for (const auto& v : data.Vertices)
            data.BindPoseAABB.Expand(v.Position);

        for (unsigned int i = 0; i < mesh->mNumFaces; i++) {
            aiFace face = mesh->mFaces[i];
            for (unsigned int j = 0; j < face.mNumIndices; j++)
                data.Indices.push_back(face.mIndices[j]);
        }

        return data;
    }

    // Skinned meshes bake the mesh node transform into vertices so they match the skeleton's coordinate space
    // (InverseBindPose is recomputed from the engine-side hierarchy).
    static MeshData ProcessSkinnedMesh(aiMesh* mesh, const aiScene* scene,
        const Skeleton& skeleton, const Mat4& meshTransform)
    {
        MeshData data;
        data.Name = mesh->mName.C_Str();
        data.MaterialIndex = mesh->mMaterialIndex;
        data.IsSkinned = true;

        Mat3 normalMatrix = ConvertToNormalMatrix(meshTransform);

        data.SkinnedVertices.reserve(mesh->mNumVertices);
        for (unsigned int i = 0; i < mesh->mNumVertices; i++) {
            SkinnedVertex vertex;

            Vec4 pos = meshTransform * Vec4(AiVec3ToGLM(mesh->mVertices[i]), 1.0f);
            vertex.Position = Vec3(pos);

            if (mesh->HasNormals())
                vertex.Normal = Math::Normalize(normalMatrix * AiVec3ToGLM(mesh->mNormals[i]));
            else
                vertex.Normal = Vec3(0.0f);

            if (mesh->mTextureCoords[0])
                vertex.TexCoord0 = Vec2(mesh->mTextureCoords[0][i].x, mesh->mTextureCoords[0][i].y);
            else
                vertex.TexCoord0 = Vec2(0.0f);

            if (mesh->mTextureCoords[1])
                vertex.TexCoord1 = Vec2(mesh->mTextureCoords[1][i].x, mesh->mTextureCoords[1][i].y);
            else
                vertex.TexCoord1 = Vec2(0.0f);

            if (mesh->HasTangentsAndBitangents())
                vertex.Tangent = Math::Normalize(normalMatrix * AiVec3ToGLM(mesh->mTangents[i]));
            else
                vertex.Tangent = Vec3(0.0f);

            // BoneIDs and BoneWeights initialized to defaults by SkinnedVertex constructor
            data.SkinnedVertices.push_back(vertex);
        }

        // Compute bind-pose AABB from vertex positions
        for (const auto& v : data.SkinnedVertices)
            data.BindPoseAABB.Expand(v.Position);

        for (unsigned int i = 0; i < mesh->mNumFaces; i++) {
            aiFace face = mesh->mFaces[i];
            for (unsigned int j = 0; j < face.mNumIndices; j++)
                data.Indices.push_back(face.mIndices[j]);
        }

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
                outMeshes.push_back(ProcessSkinnedMesh(mesh, scene, skeleton, transform));
            } else {
                outMeshes.push_back(ProcessStaticMesh(mesh, scene, transform));
            }
        }

        for (unsigned int i = 0; i < node->mNumChildren; i++) {
            ProcessNode(node->mChildren[i], scene, transform, outMeshes, isSkinned, skeleton);
        }
    }

    // Decompose a node's local matrix to TRS for its entity Transform. Deliberately uses
    // Math::Decompose (raw glm::decompose) and NOT Luth::DecomposeTransform: the latter's trailing
    // glm::conjugate inverts the rotation in this glm version, which renders imported nodes facing the
    // wrong way (the error scales with rotation angle; only axis-aligned nodes look right). The conjugate
    // is latent/masked elsewhere; animation drives skinning from the bone-matrix buffer and physics
    // reads quaternions directly, so neither round-trips a rotation through the entity Transform the way
    // node import does. Verified with a decompose->euler->reconstruct round-trip (see commit history).
    static void DecomposeNodeLocal(const Mat4& m, Vec3& t, Quat& r, Vec3& s)
    {
        Vec3 skew; Vec4 persp;
        Math::Decompose(m, s, r, t, skew, persp);
    }

    // Static-model path: preserve the DCC node hierarchy as a topological node list, store meshes
    // un-baked (mesh-local; the entity tree composes world transforms), and extract cameras + lights.
    // Axis correction + scale factor fold onto the root node's transform (mirrors how the skinned path
    // applies axis correction to the root bone, so descendants inherit it through the hierarchy).
    static void BuildStaticSceneGraph(const aiScene* scene, const Mat4& rootCorrection,
        bool importCameras, bool importLights, ModelAssetData& modelData)
    {
        // Every scene mesh processed once, un-baked. Node MeshIndices reference these by global index.
        modelData.Meshes.reserve(scene->mNumMeshes);
        for (unsigned int i = 0; i < scene->mNumMeshes; ++i)
            modelData.Meshes.push_back(ProcessStaticMesh(scene->mMeshes[i], scene, Mat4(1.0f)));

        // Cameras + lights -> defs, keyed by node name (Assimp links them to nodes by name).
        std::unordered_map<std::string, i32> cameraByName, lightByName;

        for (unsigned int i = 0; importCameras && i < scene->mNumCameras; ++i) {
            const aiCamera* cam = scene->mCameras[i];
            ModelCamera mc;
            mc.Aspect  = (cam->mAspect > 1e-4f) ? cam->mAspect : (16.0f / 9.0f);
            // mHorizontalFOV is the half horizontal angle (radians); convert to full vertical degrees.
            mc.FovYDeg = Math::Degrees(2.0f * std::atan(std::tan(cam->mHorizontalFOV) / mc.Aspect));
            mc.NearClip = cam->mClipPlaneNear;
            mc.FarClip  = cam->mClipPlaneFar;
            cameraByName[cam->mName.C_Str()] = (i32)modelData.Cameras.size();
            modelData.Cameras.push_back(mc);
        }

        for (unsigned int i = 0; importLights && i < scene->mNumLights; ++i) {
            const aiLight* light = scene->mLights[i];
            ModelLight ml;
            switch (light->mType) {
                case aiLightSource_DIRECTIONAL: ml.Type = 0; break;
                case aiLightSource_POINT:       ml.Type = 1; break;
                case aiLightSource_SPOT:
                    // Assimp cone angles are radians; treated as half-angles (glTF convention).
                    // The gatherer clamps to a sane range, so a full-angle source just imports wide.
                    ml.Type = 2;
                    ml.InnerConeAngleDeg = Math::Degrees(light->mAngleInnerCone);
                    ml.OuterConeAngleDeg = Math::Degrees(light->mAngleOuterCone);
                    break;
                default:
                    LH_LOG(Assets, warn, "ModelImporter: unsupported light type ('{}') skipped", light->mName.C_Str());
                    continue;
            }
            // Assimp folds intensity into the color magnitude; split it back out for a sane editor range.
            Vec3 color(light->mColorDiffuse.r, light->mColorDiffuse.g, light->mColorDiffuse.b);
            float maxc = std::max({ color.x, color.y, color.z });
            if (maxc > 1.0f) { ml.Intensity = maxc; color /= maxc; }
            ml.Color = color;
            if (ml.Type != 0 && light->mAttenuationQuadratic > 1e-4f)   // point + spot
                ml.Range = std::clamp(std::sqrt(1.0f / (0.01f * light->mAttenuationQuadratic)), 1.0f, 10000.0f);
            lightByName[light->mName.C_Str()] = (i32)modelData.Lights.size();
            modelData.Lights.push_back(ml);
        }

        // Walk the hierarchy depth-first -> topological order (parent always precedes its children).
        std::function<void(const aiNode*, i32)> walk = [&](const aiNode* node, i32 parentIndex) {
            ModelNode mn;
            mn.Name = node->mName.C_Str();
            mn.ParentIndex = parentIndex;

            Mat4 local = AiMat4ToGLM(node->mTransformation);
            if (parentIndex < 0) local = rootCorrection * local;
            DecomposeNodeLocal(local, mn.Translation, mn.Rotation, mn.Scale);

            for (unsigned int i = 0; i < node->mNumMeshes; ++i)
                mn.MeshIndices.push_back(node->mMeshes[i]);
            if (auto it = cameraByName.find(mn.Name); it != cameraByName.end()) mn.CameraIndex = it->second;
            if (auto it = lightByName.find(mn.Name);  it != lightByName.end())  mn.LightIndex  = it->second;

            i32 myIndex = (i32)modelData.Nodes.size();
            modelData.Nodes.push_back(std::move(mn));

            for (unsigned int i = 0; i < node->mNumChildren; ++i)
                walk(node->mChildren[i], myIndex);
        };
        walk(scene->mRootNode, -1);
    }

    // Lowercased filename stem, used to refine a texture's role beyond what the Assimp semantic type says.
    static std::string StemLower(const fs::path& p)
    {
        std::string s = p.stem().string();
        std::transform(s.begin(), s.end(), s.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return s;
    }

    static bool StemHasAny(const std::string& stem, std::initializer_list<const char*> needles)
    {
        for (const char* n : needles)
            if (stem.find(n) != std::string::npos) return true;
        return false;
    }

    // ORM / ARM / RMA packs carry occlusion in R, roughness in G, metallic in B, already canonical for the
    // decode, so they alias into both the occlusion and metalRough slots with no pixel transform.
    static bool IsOrmStem(const fs::path& p) { return StemHasAny(StemLower(p), { "_orm", "_arm", "_rma" }); }

    // DirectX normals (-Y green) need a flip; not reliably filename-detectable, so default to GL and let a
    // _dx suffix or the texture inspector override decide.
    static TextureRole InferNormalRole(const fs::path& p)
    {
        return StemHasAny(StemLower(p), { "_dx", "_directx" }) ? TextureRole::NormalDX : TextureRole::NormalGL;
    }

    // A metalRough/ORM-bound map is canonical unless its green channel is authored as glossiness.
    static TextureRole InferMetalRoughRole(const fs::path& p)
    {
        return StemHasAny(StemLower(p), { "_gloss", "_glossiness" })
             ? TextureRole::GlossToRoughness : TextureRole::LinearData;
    }

    // ---- Importer Logic ----

    struct ImportContext {
        fs::path SourcePath;
        fs::path TextureDir;
        fs::path MaterialDir;
        const aiScene* Scene;
        std::unordered_map<std::string, UUID> TexturePathToUUID;
        std::vector<UUID> MaterialUUIDs;
        bool AutoDetectRoles = true;
    };

    static void ProcessTextures(ImportContext& ctx)
    {
        if (!ctx.Scene->HasTextures() && !ctx.Scene->HasMaterials()) return;

        if (!fs::exists(ctx.TextureDir))
            fs::create_directories(ctx.TextureDir);

        // Extract embedded textures
        for (unsigned int i = 0; i < ctx.Scene->mNumTextures; ++i)
        {
            aiTexture* texture = ctx.Scene->mTextures[i];
            std::string fileName = ctx.SourcePath.stem().string() + "_Tex_" + std::to_string(i) + ".png";
            
            // If hint is available, use it for extension
            if (texture->achFormatHint[0] != 0) {
                fileName = ctx.SourcePath.stem().string() + "_Tex_" + std::to_string(i) + "." + std::string(texture->achFormatHint);
            }

            fs::path destPath = ctx.TextureDir / fileName;

            if (!fs::exists(destPath)) {
                if (texture->mHeight == 0) {
                    // Compressed format (jpg/png), write directly
                    std::ofstream file(destPath, std::ios::binary);
                    file.write((const char*)texture->pcData, texture->mWidth);
                } else {
                    // Raw ARGB8888, would need encoding (skip for now or use stbi_write)
                    LH_LOG(Assets, warn, "Raw embedded textures not fully supported yet: {0}", fileName);
                    continue;
                }
            }

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

        UUID matUUID = AssetDatabase::GetUUID(matPath);
        if (matUUID.IsValid()) return matUUID;

        // Create new Material
        nlohmann::json matJson;
        UUID pbrUUID = AssetDatabase::GetUUID(FileSystem::EngineAssetsPath("shaders/pbr_vert.slang"));
        matJson["shader"] = pbrUUID.IsValid() ? pbrUUID.ToString() : "";
        // Render mode (Opaque=0/Cutout=1/Transparent=2; Fade=3 is editor-only): glTF alphaMode wins,
        // else opacity<1 -> Transparent. The opacity>0.001 floor dodges the FBX "0 means default" quirk.
        int renderMode = 0;
        float opacity = 1.0f;
        aiMat->Get(AI_MATKEY_OPACITY, opacity);
        bool translucentOpacity = (opacity > 0.001f && opacity < 1.0f);

        aiString alphaMode;
        if (aiMat->Get(AI_MATKEY_GLTF_ALPHAMODE, alphaMode) == AI_SUCCESS) {
            std::string mode = alphaMode.C_Str();
            if (mode == "MASK")       renderMode = 1;
            else if (mode == "BLEND") renderMode = 2;
        }
        else if (translucentOpacity) {
            renderMode = 2;
        }
        matJson["render_mode"] = renderMode;

        float alphaCutoff;
        if (aiMat->Get(AI_MATKEY_GLTF_ALPHACUTOFF, alphaCutoff) == AI_SUCCESS)
            matJson["alpha_cutoff"] = alphaCutoff;

        // Two-sided -> CullMode::None (Back=0, Front=1, None=2). Leave the default (Back) otherwise.
        int twoSided = 0;
        if (aiMat->Get(AI_MATKEY_TWOSIDED, twoSided) == AI_SUCCESS && twoSided != 0)
            matJson["cull_mode"] = 2;

        // Material factors -> direct keys (color/metalness/roughness). The u_* uniform channel never
        // reached the GPU (no Set-1 block in pbr), so importing into it silently dropped the factors.
        aiColor4D color;
        if (aiMat->Get(AI_MATKEY_BASE_COLOR, color) == AI_SUCCESS ||
            aiMat->Get(AI_MATKEY_COLOR_DIFFUSE, color) == AI_SUCCESS) {
            // Fold a separate opacity scalar (FBX/OBJ) into base-color alpha when the source carries one.
            float a = translucentOpacity ? opacity : color.a;
            matJson["color"] = { color.r, color.g, color.b, a };
        }

        float floatVal;
        if (aiMat->Get(AI_MATKEY_METALLIC_FACTOR, floatVal) == AI_SUCCESS)  matJson["metalness"] = floatVal;
        if (aiMat->Get(AI_MATKEY_ROUGHNESS_FACTOR, floatVal) == AI_SUCCESS) matJson["roughness"] = floatVal;

        // Textures: resolve each Assimp slot to a UUID + source path, classify a TextureRole, then route
        // into the bounded material slots. Packed layouts (ORM alias, separate metal+rough bake) land here
        // so material.slang's fixed-swizzle decode always reads canonical channels with no shader change.
        matJson["textures"] = nlohmann::json::array();

        struct Resolved { UUID uuid = UUID::Invalid(); fs::path path; int uv = 0; };

        auto ResolveSlot = [&](aiTextureType aiType, MapType reportType) -> Resolved {
            if (aiMat->GetTextureCount(aiType) == 0) return {};
            aiString path;
            if (aiMat->GetTexture(aiType, 0, &path) != AI_SUCCESS) return {};
            std::string pathStr = path.C_Str();

            Resolved t;
            if (ctx.TexturePathToUUID.count(pathStr)) {
                t.uuid = ctx.TexturePathToUUID[pathStr];           // embedded ("*0", "*1", ...)
                t.path = AssetDatabase::GetMetadata(t.uuid).Path;
            }
            else {
                ResolveResult r = ResolveTexturePath(ctx.SourcePath.parent_path(), pathStr);
                if (r.ResolvedPath.empty()) {
                    s_LastImportReport.Unresolved.push_back({ matName, matPath, pathStr, reportType });
                    return {};
                }
                t.uuid = AssetDatabase::GetUUID(r.ResolvedPath);
                if (!t.uuid.IsValid()) {
                    t.uuid = MetaFile::Create(r.ResolvedPath, AssetType::Texture);
                    AssetDatabase::RegisterAsset(r.ResolvedPath, t.uuid, AssetType::Texture);
                }
                t.path = r.ResolvedPath;
                if (r.Strategy != "direct")
                    LH_LOG(Assets, info, "ModelImporter: Found texture via '{0}' strategy: {1} -> {2}",
                        r.Strategy, pathStr, r.ResolvedPath.filename().string());
            }
            // UV set from the DCC; the engine carries two (TexCoord0/1), so clamp >0 to 1.
            int uvChannel = 0;
            aiMat->Get(AI_MATKEY_UVWSRC(aiType, 0), uvChannel);
            t.uv = (uvChannel <= 0) ? 0 : 1;
            return t;
        };

        auto AddNode = [&](MapType luthType, const Resolved& t) {
            if (!t.uuid.IsValid()) return;
            for (const auto& existing : matJson["textures"])       // one texture per canonical slot
                if (existing["type"].get<int>() == (int)luthType) return;
            nlohmann::json node;
            node["type"] = (int)luthType;
            node["uuid"] = t.uuid.ToString();
            node["uv"] = t.uv;
            node["useTexture"] = true;
            matJson["textures"].push_back(node);
        };

        // Stamp the inferred role into the texture's .meta unless auto-detect is off or a role is already
        // set. On a fresh stamp, drop any stale artifact so the next load reimports with the transform.
        auto StampRole = [&](const Resolved& t, TextureRole role) {
            if (!ctx.AutoDetectRoles || !t.uuid.IsValid() || t.path.empty()) return;
            fs::path metaPath = t.path.string() + ".meta";
            MetaFile meta(t.uuid);
            meta.Load(metaPath);
            auto& ts = meta.GetTypeSettings();
            if (ts.contains("role")) return;                       // never clobber a prior / user-set role
            ts["role"] = (int)role;
            meta.Save(metaPath);
            fs::path artifact = AssetDatabase::GetArtifactPath(t.uuid);
            if (fs::exists(artifact)) fs::remove(artifact);
        };

        // Base color / normal / emissive: straight one-to-one routing.
        Resolved diffuse = ResolveSlot(aiTextureType_BASE_COLOR, MapType::Diffuse);
        if (!diffuse.uuid.IsValid()) diffuse = ResolveSlot(aiTextureType_DIFFUSE, MapType::Diffuse);
        StampRole(diffuse, TextureRole::Color);
        AddNode(MapType::Diffuse, diffuse);

        Resolved normal = ResolveSlot(aiTextureType_NORMALS, MapType::Normal);
        if (!normal.uuid.IsValid()) normal = ResolveSlot(aiTextureType_NORMAL_CAMERA, MapType::Normal);
        StampRole(normal, InferNormalRole(normal.path));
        AddNode(MapType::Normal, normal);

        Resolved emissive = ResolveSlot(aiTextureType_EMISSIVE, MapType::Emissive);
        StampRole(emissive, TextureRole::Color);
        AddNode(MapType::Emissive, emissive);

        // Occlusion: a dedicated AO map; an ORM metalRough may also alias into this slot below.
        Resolved occlusion = ResolveSlot(aiTextureType_AMBIENT_OCCLUSION, MapType::Occlusion);
        if (!occlusion.uuid.IsValid()) occlusion = ResolveSlot(aiTextureType_LIGHTMAP, MapType::Occlusion);
        StampRole(occlusion, TextureRole::LinearData);
        AddNode(MapType::Occlusion, occlusion);

        // Metal-rough: the crux. glTF reports the combined map under METALNESS and/or DIFFUSE_ROUGHNESS
        // (same file). Separate single-channel files are baked into one packed map; an ORM pack aliases
        // into the occlusion slot too (its R channel feeds the occlusion read).
        {
            Resolved metal = ResolveSlot(aiTextureType_METALNESS, MapType::Metalness);
            Resolved rough = ResolveSlot(aiTextureType_DIFFUSE_ROUGHNESS, MapType::Metalness);
            bool sameFile = metal.uuid.IsValid() && rough.uuid.IsValid() && metal.uuid == rough.uuid;

            auto routeCombined = [&](const Resolved& mr) {
                StampRole(mr, InferMetalRoughRole(mr.path));
                AddNode(MapType::Metalness, mr);
                if (!occlusion.uuid.IsValid() && IsOrmStem(mr.path))
                    AddNode(MapType::Occlusion, mr);
            };

            if (metal.uuid.IsValid() && (sameFile || !rough.uuid.IsValid()))
                routeCombined(metal);
            else if (rough.uuid.IsValid() && !metal.uuid.IsValid())
                routeCombined(rough);
            else if (metal.uuid.IsValid() && rough.uuid.IsValid())  // separate files -> bake one packed map
            {
                UUID baked = TextureBaker::BakeMetalRough(ctx.TextureDir, matName,
                                                          rough.path, rough.uuid, metal.path, metal.uuid);
                if (baked.IsValid()) {
                    Resolved t; t.uuid = baked; t.uv = rough.uv;
                    AddNode(MapType::Metalness, t);
                }
                else {
                    StampRole(rough, TextureRole::LinearData);
                    AddNode(MapType::Metalness, rough);
                    LH_LOG(Assets, warn, "ModelImporter: '{0}' metal+rough bake failed; routed roughness only", matName);
                    s_LastImportReport.Degraded.push_back({ matName, matPath,
                        "separate metal+rough bake failed: roughness routed, metallic from factor" });
                }
            }
        }

        // Spec-gloss workflow: a material with a specular(-glossiness) map and no metal-rough is converted
        // to metal-rough (+ baseColor) so the decode reads canonical channels. Lossy by nature; see history.
        bool hasMetalRough = false;
        for (const auto& t : matJson["textures"])
            if (t["type"].get<int>() == (int)MapType::Metalness) { hasMetalRough = true; break; }

        if (!hasMetalRough && aiMat->GetTextureCount(aiTextureType_SPECULAR) > 0)
        {
            Resolved spec = ResolveSlot(aiTextureType_SPECULAR, MapType::Metalness);
            if (spec.uuid.IsValid())
            {
                TextureBaker::SpecGlossInputs sgIn;
                sgIn.specGlossSrc = spec.path; sgIn.specGlossUuid = spec.uuid;
                if (diffuse.uuid.IsValid()) { sgIn.diffuseSrc = diffuse.path; sgIn.diffuseUuid = diffuse.uuid; }

                aiColor4D dc;
                if (aiMat->Get(AI_MATKEY_COLOR_DIFFUSE, dc) == AI_SUCCESS) {
                    sgIn.diffuseFactor[0] = dc.r; sgIn.diffuseFactor[1] = dc.g;
                    sgIn.diffuseFactor[2] = dc.b; sgIn.diffuseFactor[3] = dc.a;
                }
                aiColor3D sc;
                if (aiMat->Get(AI_MATKEY_COLOR_SPECULAR, sc) == AI_SUCCESS) {
                    sgIn.specularFactor[0] = sc.r; sgIn.specularFactor[1] = sc.g; sgIn.specularFactor[2] = sc.b;
                }
                aiMat->Get(AI_MATKEY_GLOSSINESS_FACTOR, sgIn.glossinessFactor);

                TextureBaker::SpecGlossResult res =
                    TextureBaker::BakeSpecGlossToMetalRough(ctx.TextureDir, matName, sgIn);
                if (res.metalRough.IsValid())
                {
                    Resolved mr; mr.uuid = res.metalRough; mr.uv = spec.uv;
                    AddNode(MapType::Metalness, mr);

                    if (res.baseColor.IsValid())
                    {
                        // The converted baseColor folds the diffuse factor in, so swap the raw diffuse node
                        // for it and neutralize the scalar color (the decode multiplies color * baseColor).
                        auto& arr = matJson["textures"];
                        for (auto it = arr.begin(); it != arr.end(); ++it)
                            if ((*it)["type"].get<int>() == (int)MapType::Diffuse) { arr.erase(it); break; }
                        Resolved bc; bc.uuid = res.baseColor; bc.uv = diffuse.uv;
                        AddNode(MapType::Diffuse, bc);
                        matJson["color"] = { 1.0f, 1.0f, 1.0f, 1.0f };
                    }
                    LH_LOG(Assets, info, "ModelImporter: '{0}' converted spec-gloss to metal-rough", matName);
                }
                else
                {
                    float gloss = 1.0f;
                    aiMat->Get(AI_MATKEY_GLOSSINESS_FACTOR, gloss);
                    matJson["roughness"] = 1.0f - gloss;
                    LH_LOG(Assets, warn, "ModelImporter: '{0}' spec-gloss bake failed; roughness from gloss factor", matName);
                    s_LastImportReport.Degraded.push_back({ matName, matPath,
                        "spec-gloss bake failed: roughness from factor, metallic from factor" });
                }
            }
        }

        // Emissive factor -> the direct "emissive" key (rgb factor, a strength), NOT the dead u_*
        // uniform channel. Default to white when only a resolved emissive texture is present, so glTF
        // assets that leave the factor at default but ship a texture still emit.
        {
            aiColor3D emissive(0.0f, 0.0f, 0.0f);
            bool srcFactor = aiMat->Get(AI_MATKEY_COLOR_EMISSIVE, emissive) == AI_SUCCESS
                          && (emissive.r > 0.0f || emissive.g > 0.0f || emissive.b > 0.0f);
            bool hasEmissiveNode = false;
            for (const auto& t : matJson["textures"])
                if (t["type"].get<int>() == (int)MapType::Emissive) { hasEmissiveNode = true; break; }
            if (srcFactor)
                matJson["emissive"] = { emissive.r, emissive.g, emissive.b, 1.0f };
            else if (hasEmissiveNode)
                matJson["emissive"] = { 1.0f, 1.0f, 1.0f, 1.0f };
        }

        if (!fs::exists(ctx.MaterialDir)) fs::create_directories(ctx.MaterialDir);
        
        std::ofstream file(matPath);
        file << matJson.dump(4);
        file.close();

        matUUID = MetaFile::Create(matPath, AssetType::Material);
        AssetDatabase::RegisterAsset(matPath, matUUID, AssetType::Material);
        
        return matUUID;
    }

    bool ModelImporter::Import(const std::filesystem::path& source, const std::filesystem::path& destination)
    {
        LH_PROFILE_FUNCTION();

        s_LastImportReport.Clear();
        s_LastImportReport.ModelPath = source;

        // Load import settings from .meta file
        ModelImportSettings settings;
        {
            fs::path metaPath = source.string() + ".meta";
            MetaFile meta(UUID{});
            if (meta.Load(metaPath))
                settings = ModelImportSettings::FromJson(meta.GetTypeSettings());
        }

        ImportContext ctx;
        ctx.SourcePath = source;
        ctx.TextureDir = source.parent_path() / (source.stem().string() + "_Textures");
        ctx.MaterialDir = source.parent_path() / (source.stem().string() + "_Materials");
        ctx.AutoDetectRoles = settings.AutoDetectTextureRoles;

        Assimp::Importer importer;
        importer.SetPropertyBool(AI_CONFIG_IMPORT_FBX_PRESERVE_PIVOTS, false);

        // Build Assimp post-process flags from settings
        u32 flags = aiProcess_Triangulate | aiProcess_FlipUVs
            | aiProcess_JoinIdenticalVertices | aiProcess_LimitBoneWeights;
        if (settings.ImportNormals)  flags |= aiProcess_GenSmoothNormals;
        if (settings.ImportTangents) flags |= aiProcess_CalcTangentSpace;
        if (settings.OptimizeMesh)   flags |= aiProcess_OptimizeMeshes;

        const aiScene* scene;
        {
            LH_PROFILE_SCOPE("ParseScene");
            scene = importer.ReadFile(source.string(), flags);
        }

        if (!scene || scene->mFlags & AI_SCENE_FLAGS_INCOMPLETE || !scene->mRootNode) {
            LH_LOG(Assets, error, "ModelImporter: Failed to load model {0} : {1}", source.string(), importer.GetErrorString());
            return false;
        }

        ctx.Scene = scene;

        ProcessTextures(ctx);

        {
            LH_PROFILE_SCOPE("ProcessMaterials");
            ctx.MaterialUUIDs.resize(scene->mNumMaterials);
            for (unsigned int i = 0; i < scene->mNumMaterials; i++) {
                ctx.MaterialUUIDs[i] = ProcessMaterial(ctx, scene->mMaterials[i], i);
            }
        }

        if (s_LastImportReport.HasUnresolved()) {
            LH_LOG(Assets, warn, "ModelImporter: {} texture(s) could not be resolved for '{}'."
                " Use the Texture Remap dialog to assign them.",
                s_LastImportReport.Unresolved.size(), source.filename().string());
        }

        // Extract skeleton if the model has bones
        ModelAssetData modelData;
        Mat4 axisCorrection = AxisCorrectionMatrix(scene);

        // Apply scale factor to axis correction
        if (std::abs(settings.ScaleFactor - 1.0f) > 1e-4f)
            axisCorrection = Math::Scale(axisCorrection, Vec3(settings.ScaleFactor));

        bool isSkinned = SceneHasBones(scene);
        modelData.IsSkinned = isSkinned;

        if (isSkinned) {
            // Mesh-space correction captures DCC-baked rotations on mesh nodes that mOffsetMatrix (mesh-local space) misses.
            Mat4 meshSpaceCorrection(1.0f);
            using Mode = ModelImportSettings::MeshTransformMode;
            if (settings.SkinMeshTransform == Mode::Bake) {
                meshSpaceCorrection = ComputeMeshSpaceCorrection(scene);
            } else if (settings.SkinMeshTransform == Mode::Auto) {
                Mat4 candidate = ComputeMeshSpaceCorrection(scene);
                if (!IsNearIdentity(candidate))
                    meshSpaceCorrection = candidate;
            }
            // Mode::Identity leaves it as mat4(1.0f) (legacy behavior)

            ExtractSkeleton(scene, modelData.SkeletonData, axisCorrection, meshSpaceCorrection);
            RecomputeInverseBindPoses(modelData.SkeletonData);

            std::vector<AnimationClip> extractedClips;
            ExtractAnimationClips(scene, modelData.SkeletonData, extractedClips);

            if (settings.ExtractClipsAsSeparateAssets && !extractedClips.empty())
            {
                fs::path animDir = source.parent_path() / (source.stem().string() + "_Animations");
                if (!fs::exists(animDir))
                    fs::create_directories(animDir);

                std::unordered_set<std::string> usedNames;
                modelData.AnimationClipUUIDs.reserve(extractedClips.size());

                for (const auto& clip : extractedClips)
                {
                    std::string clipName = clip.Name.empty() ? "Animation" : clip.Name;
                    std::replace(clipName.begin(), clipName.end(), ':',  '_');
                    std::replace(clipName.begin(), clipName.end(), '/',  '_');
                    std::replace(clipName.begin(), clipName.end(), '\\', '_');
                    std::replace(clipName.begin(), clipName.end(), '|',  '_');
                    std::replace(clipName.begin(), clipName.end(), '?',  '_');
                    std::replace(clipName.begin(), clipName.end(), '*',  '_');
                    std::replace(clipName.begin(), clipName.end(), '<',  '_');
                    std::replace(clipName.begin(), clipName.end(), '>',  '_');
                    std::replace(clipName.begin(), clipName.end(), '"',  '_');

                    // Uniquify within this import: duplicate clip names from a single FBX are rare but
                    // uniqueness is required for stable .anim file paths.
                    std::string finalName = clipName;
                    int counter = 1;
                    while (usedNames.count(finalName)) {
                        finalName = clipName + "_" + std::to_string(counter++);
                    }
                    usedNames.insert(finalName);

                    fs::path clipPath = animDir / (finalName + ".anim");

                    AnimationAssetData animData;
                    animData.Clip = clip;
                    if (!AssetSerializer::SerializeAnimation(clipPath, animData)) {
                        LH_LOG(Assets, warn, "ModelImporter: Failed to write clip '{}'", clipPath.string());
                        continue;
                    }

                    // Get-or-create UUID; keep the existing one across re-imports so scene references stay stable when only the FBX changed.
                    UUID clipUUID = AssetDatabase::GetUUID(clipPath);
                    if (!clipUUID.IsValid())
                        clipUUID = MetaFile::Create(clipPath, AssetType::Animation);
                    AssetDatabase::RegisterAsset(clipPath, clipUUID, AssetType::Animation);

                    modelData.AnimationClipUUIDs.push_back(clipUUID);
                }
            }
        }

        // Process geometry: skinned keeps the baked/flattened path (verts in skeleton space, bone-driven);
        // static reconstructs the node graph with un-baked meshes + cameras + lights.
        {
            LH_PROFILE_SCOPE("ProcessGeometry");
            if (isSkinned)
                ProcessNode(scene->mRootNode, scene, axisCorrection, modelData.Meshes, isSkinned, modelData.SkeletonData);
            else
                BuildStaticSceneGraph(scene, axisCorrection, settings.ImportCameras, settings.ImportLights, modelData);
        }

        // Wind-deformable opt-in applies to STATIC meshes only (skinned meshes deform via skinning).
        if (settings.MarkDeformable)
            for (auto& m : modelData.Meshes)
                if (!m.IsSkinned) m.IsDeformable = true;

        modelData.Materials = ctx.MaterialUUIDs;

        {
            LH_PROFILE_SCOPE("SerializeModel");
            return AssetSerializer::SerializeModel(destination, modelData);
        }
    }
}