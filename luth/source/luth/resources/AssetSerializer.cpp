#include "luthpch.h"
#include "AssetSerializer.h"
#include "luth/resources/importers/TextureImporter.h"
#include "luth/resources/importers/ModelImporter.h"
#include "luth/resources/importers/MaterialImporter.h"
#include "luth/resources/importers/PhysicsMaterialImporter.h"
#include "luth/resources/importers/ShaderImporter.h"
#include "luth/resources/importers/AnimationClipImporter.h"
#include <fstream>

namespace Luth
{
    // Serialize to a temp sibling then atomically rename into place. std::ofstream truncates-then-streams,
    // so an in-place write leaves fs::exists(path)==true over partial bytes for the whole write window and a
    // racing reader torn-reads. Writing {path}.tmp then renaming makes the artifact appear atomically (same
    // dir -> same volume), and a crash mid-write leaves the prior artifact intact instead of a truncated stub.
    template <typename WriteFn>
    static bool SerializeAtomic(const fs::path& path, WriteFn&& writeFn)
    {
        fs::path tmp = path;
        tmp += ".tmp";

        bool ok = false;
        {
            std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
            if (out.is_open())
                ok = writeFn(out) && out.good();
        }   // flush + close before the rename

        std::error_code ec;
        if (!ok) { fs::remove(tmp, ec); return false; }

        fs::rename(tmp, path, ec);
        if (ec) { fs::remove(tmp, ec); return false; }
        return true;
    }

    bool AssetSerializer::SerializeTexture(const fs::path& path, const TextureAssetData& data)
    {
        LH_PROFILE_FUNCTION();
        return SerializeAtomic(path, [&](std::ofstream& out) {
            AssetHeader header;
            header.Type = AssetType::Texture;
            header.Version = 2; // V2: BCn payload + MipLevels. V1 artifacts fail the deserialize gate.
            out.write((char*)&header, sizeof(AssetHeader));

            TextureHeader texHeader;
            texHeader.Width = data.Width;
            texHeader.Height = data.Height;
            texHeader.Format = (u32)data.Format;
            texHeader.SizeBytes = (u32)data.Pixels.size();
            texHeader.GenerateMipmaps = data.Settings.GenerateMipmaps ? 1 : 0;
            texHeader.WrapMode = (u32)data.Settings.WrapMode;
            texHeader.MinFilter = (u32)data.Settings.MinFilter;
            texHeader.MagFilter = (u32)data.Settings.MagFilter;
            texHeader.MipLevels = data.MipLevels;
            out.write((char*)&texHeader, sizeof(TextureHeader));

            out.write((char*)data.Pixels.data(), data.Pixels.size());
            return true;
        });
    }

    bool AssetSerializer::DeserializeTexture(const fs::path& path, TextureAssetData& outData)
    {
        LH_PROFILE_FUNCTION();
        std::ifstream in(path, std::ios::binary);
        if (!in.is_open()) return false;

        AssetHeader header;
        in.read((char*)&header, sizeof(AssetHeader));
        if (header.Type != AssetType::Texture) return false;
        // Reject pre-BCn (V1) artifacts so the loader's force-reimport self-heals them (AssetManager).
        if (header.Version != 2) return false;

        TextureHeader texHeader;
        in.read((char*)&texHeader, sizeof(TextureHeader));

        outData.Width = texHeader.Width;
        outData.Height = texHeader.Height;
        outData.Format = (TextureFormat)texHeader.Format;
        outData.MipLevels = texHeader.MipLevels;
        outData.Settings.GenerateMipmaps = texHeader.GenerateMipmaps != 0;
        outData.Settings.WrapMode = (TextureWrapMode)texHeader.WrapMode;
        outData.Settings.MinFilter = (TextureFilterMode)texHeader.MinFilter;
        outData.Settings.MagFilter = (TextureFilterMode)texHeader.MagFilter;
        outData.Pixels.resize(texHeader.SizeBytes);
        in.read((char*)outData.Pixels.data(), texHeader.SizeBytes);

        return true;
    }

    // ---- Skeleton/Animation binary helpers ----

    static void WriteString(std::ofstream& out, const std::string& str)
    {
        u32 len = (u32)str.size();
        out.write((const char*)&len, sizeof(u32));
        out.write(str.data(), len);
    }

    static std::string ReadString(std::ifstream& in)
    {
        u32 len;
        in.read((char*)&len, sizeof(u32));
        std::string str(len, '\0');
        in.read(str.data(), len);
        return str;
    }

    static void WriteSkeleton(std::ofstream& out, const Skeleton& skeleton)
    {
        for (const auto& bone : skeleton.Bones) {
            WriteString(out, bone.Name);
            out.write((const char*)&bone.ParentIndex, sizeof(i32));
            out.write((const char*)&bone.InverseBindPose, sizeof(Mat4));
            out.write((const char*)&bone.LocalBindPose, sizeof(Mat4));
        }
    }

    static void ReadSkeleton(std::ifstream& in, Skeleton& skeleton, u32 boneCount)
    {
        skeleton.Bones.resize(boneCount);
        for (u32 i = 0; i < boneCount; ++i) {
            auto& bone = skeleton.Bones[i];
            bone.Name = ReadString(in);
            in.read((char*)&bone.ParentIndex, sizeof(i32));
            in.read((char*)&bone.InverseBindPose, sizeof(Mat4));
            in.read((char*)&bone.LocalBindPose, sizeof(Mat4));
            skeleton.BoneNameToIndex[bone.Name] = static_cast<i32>(i);
        }
    }

    static void WriteAnimationClip(std::ofstream& out, const AnimationClip& clip)
    {
        WriteString(out, clip.Name);
        out.write((const char*)&clip.Duration, sizeof(f32));
        out.write((const char*)&clip.TicksPerSecond, sizeof(f32));
        u32 hasRootMotion = clip.HasRootMotion ? 1 : 0;
        out.write((const char*)&hasRootMotion, sizeof(u32));

        u32 trackCount = (u32)clip.Tracks.size();
        out.write((const char*)&trackCount, sizeof(u32));

        for (const auto& track : clip.Tracks) {
            out.write((const char*)&track.BoneIndex, sizeof(i32));

            u32 posCount = (u32)track.Positions.size();
            u32 rotCount = (u32)track.Rotations.size();
            u32 scaleCount = (u32)track.Scales.size();
            out.write((const char*)&posCount, sizeof(u32));
            out.write((const char*)&rotCount, sizeof(u32));
            out.write((const char*)&scaleCount, sizeof(u32));

            out.write((const char*)track.Positions.data(), posCount * sizeof(VectorKey));
            out.write((const char*)track.Rotations.data(), rotCount * sizeof(QuatKey));
            out.write((const char*)track.Scales.data(), scaleCount * sizeof(VectorKey));
        }

        u32 eventCount = (u32)clip.Events.size();
        out.write((const char*)&eventCount, sizeof(u32));
        for (const auto& event : clip.Events) {
            out.write((const char*)&event.Time, sizeof(f32));
            WriteString(out, event.Name);
        }
    }

    static void ReadAnimationClip(std::ifstream& in, AnimationClip& clip)
    {
        clip.Name = ReadString(in);
        in.read((char*)&clip.Duration, sizeof(f32));
        in.read((char*)&clip.TicksPerSecond, sizeof(f32));
        u32 hasRootMotion;
        in.read((char*)&hasRootMotion, sizeof(u32));
        clip.HasRootMotion = (hasRootMotion != 0);

        u32 trackCount;
        in.read((char*)&trackCount, sizeof(u32));
        clip.Tracks.resize(trackCount);

        for (u32 ti = 0; ti < trackCount; ++ti) {
            auto& track = clip.Tracks[ti];
            in.read((char*)&track.BoneIndex, sizeof(i32));

            u32 posCount, rotCount, scaleCount;
            in.read((char*)&posCount, sizeof(u32));
            in.read((char*)&rotCount, sizeof(u32));
            in.read((char*)&scaleCount, sizeof(u32));

            track.Positions.resize(posCount);
            track.Rotations.resize(rotCount);
            track.Scales.resize(scaleCount);

            in.read((char*)track.Positions.data(), posCount * sizeof(VectorKey));
            in.read((char*)track.Rotations.data(), rotCount * sizeof(QuatKey));
            in.read((char*)track.Scales.data(), scaleCount * sizeof(VectorKey));
        }

        u32 eventCount;
        in.read((char*)&eventCount, sizeof(u32));
        clip.Events.resize(eventCount);
        for (u32 ei = 0; ei < eventCount; ++ei) {
            in.read((char*)&clip.Events[ei].Time, sizeof(f32));
            clip.Events[ei].Name = ReadString(in);
        }
    }

    static void WriteAnimationClips(std::ofstream& out, const std::vector<AnimationClip>& clips)
    {
        for (const auto& clip : clips) WriteAnimationClip(out, clip);
    }

    static void ReadAnimationClips(std::ifstream& in, std::vector<AnimationClip>& clips, u32 clipCount)
    {
        clips.resize(clipCount);
        for (u32 ci = 0; ci < clipCount; ++ci) ReadAnimationClip(in, clips[ci]);
    }

    // ---- Scene-graph binary helpers (V4) ----

    static void WriteSceneGraph(std::ofstream& out, const ModelAssetData& data)
    {
        for (const auto& node : data.Nodes) {
            WriteString(out, node.Name);
            out.write((const char*)&node.ParentIndex, sizeof(i32));
            out.write((const char*)&node.Translation, sizeof(Vec3));
            out.write((const char*)&node.Rotation, sizeof(Quat));
            out.write((const char*)&node.Scale, sizeof(Vec3));
            u32 meshCount = (u32)node.MeshIndices.size();
            out.write((const char*)&meshCount, sizeof(u32));
            out.write((const char*)node.MeshIndices.data(), meshCount * sizeof(u32));
            out.write((const char*)&node.CameraIndex, sizeof(i32));
            out.write((const char*)&node.LightIndex, sizeof(i32));
        }
        // ModelCamera / ModelLight are trivially-copyable PODs (round-trips with the matching read).
        for (const auto& cam : data.Cameras)   out.write((const char*)&cam,   sizeof(ModelCamera));
        for (const auto& light : data.Lights)  out.write((const char*)&light, sizeof(ModelLight));
    }

    static void ReadSceneGraph(std::ifstream& in, ModelAssetData& data,
        u32 nodeCount, u32 cameraCount, u32 lightCount)
    {
        data.Nodes.resize(nodeCount);
        for (u32 i = 0; i < nodeCount; ++i) {
            auto& node = data.Nodes[i];
            node.Name = ReadString(in);
            in.read((char*)&node.ParentIndex, sizeof(i32));
            in.read((char*)&node.Translation, sizeof(Vec3));
            in.read((char*)&node.Rotation, sizeof(Quat));
            in.read((char*)&node.Scale, sizeof(Vec3));
            u32 meshCount = 0;
            in.read((char*)&meshCount, sizeof(u32));
            node.MeshIndices.resize(meshCount);
            in.read((char*)node.MeshIndices.data(), meshCount * sizeof(u32));
            in.read((char*)&node.CameraIndex, sizeof(i32));
            in.read((char*)&node.LightIndex, sizeof(i32));
        }
        data.Cameras.resize(cameraCount);
        for (u32 i = 0; i < cameraCount; ++i) in.read((char*)&data.Cameras[i], sizeof(ModelCamera));
        data.Lights.resize(lightCount);
        for (u32 i = 0; i < lightCount; ++i)  in.read((char*)&data.Lights[i], sizeof(ModelLight));
    }

    // ---- Model Serialization ----

    bool AssetSerializer::SerializeModel(const fs::path& path, const ModelAssetData& data)
    {
        LH_PROFILE_FUNCTION();
        return SerializeAtomic(path, [&](std::ofstream& out) {
            AssetHeader header;
            header.Version = 6; // V6: Vertex gains tangent.w handedness sign + Vec4 color (was V5 per-mesh IsDeformable)
            header.Type = AssetType::Model;
            out.write((const char*)&header, sizeof(AssetHeader));

            ModelHeader modelHeader;
            modelHeader.MeshCount = (u32)data.Meshes.size();
            modelHeader.MaterialCount = (u32)data.Materials.size();
            modelHeader.IsSkinned = data.IsSkinned ? 1 : 0;
            modelHeader.BoneCount = data.SkeletonData.BoneCount();
            modelHeader.AnimationCount = (u32)data.AnimationClipUUIDs.size();
            modelHeader.NodeCount = (u32)data.Nodes.size();
            modelHeader.CameraCount = (u32)data.Cameras.size();
            modelHeader.LightCount = (u32)data.Lights.size();
            out.write((const char*)&modelHeader, sizeof(ModelHeader));

            // Write Materials
            out.write((const char*)data.Materials.data(), data.Materials.size() * sizeof(UUID));

            // Write Meshes
            for (const auto& mesh : data.Meshes)
            {
                WriteString(out, mesh.Name);

                MeshHeader meshHeader;
                meshHeader.IsSkinned = mesh.IsSkinned ? 1 : 0;
                meshHeader.IsDeformable = mesh.IsDeformable ? 1 : 0;
                meshHeader.IndexCount = (u32)mesh.Indices.size();
                meshHeader.MaterialIndex = mesh.MaterialIndex;

                if (mesh.IsSkinned) {
                    meshHeader.VertexCount = (u32)mesh.SkinnedVertices.size();
                    out.write((const char*)&meshHeader, sizeof(MeshHeader));
                    out.write((const char*)mesh.SkinnedVertices.data(), mesh.SkinnedVertices.size() * sizeof(SkinnedVertex));
                } else {
                    meshHeader.VertexCount = (u32)mesh.Vertices.size();
                    out.write((const char*)&meshHeader, sizeof(MeshHeader));
                    out.write((const char*)mesh.Vertices.data(), mesh.Vertices.size() * sizeof(Vertex));
                }

                out.write((const char*)mesh.Indices.data(), mesh.Indices.size() * sizeof(u32));
            }

            // Write Skeleton
            if (modelHeader.BoneCount > 0) {
                WriteSkeleton(out, data.SkeletonData);
            }

            // V3: clip UUIDs (was inline clip data in V2)
            if (modelHeader.AnimationCount > 0) {
                out.write((const char*)data.AnimationClipUUIDs.data(),
                    modelHeader.AnimationCount * sizeof(UUID));
            }

            // V4: scene graph (nodes + cameras + lights), appended last
            WriteSceneGraph(out, data);

            return true;
        });
    }

    bool AssetSerializer::DeserializeModel(const fs::path& path, ModelAssetData& outData)
    {
        LH_PROFILE_FUNCTION();
        std::ifstream in(path, std::ios::binary);
        if (!in.is_open()) return false;

        AssetHeader header;
        in.read((char*)&header, sizeof(AssetHeader));
        if (header.Type != AssetType::Model) return false;

        // V6 schema: Vertex gains tangent.w handedness sign + Vec4 color. Older artifacts are rejected so
        // they get re-imported under the new schema on first load (mirrors the Shader V1->V2 reject pattern).
        if (header.Version != 6) return false;

        ModelHeader modelHeader;
        in.read((char*)&modelHeader, sizeof(ModelHeader));

        outData.IsSkinned = (modelHeader.IsSkinned != 0);

        // Read Materials
        outData.Materials.resize(modelHeader.MaterialCount);
        in.read((char*)outData.Materials.data(), modelHeader.MaterialCount * sizeof(UUID));

        // Read Meshes
        outData.Meshes.resize(modelHeader.MeshCount);
        for (u32 i = 0; i < modelHeader.MeshCount; i++)
        {
            auto& mesh = outData.Meshes[i];
            mesh.Name = ReadString(in);

            MeshHeader meshHeader;
            in.read((char*)&meshHeader, sizeof(MeshHeader));
            mesh.MaterialIndex = meshHeader.MaterialIndex;
            mesh.IsSkinned = (meshHeader.IsSkinned != 0);
            mesh.IsDeformable = (meshHeader.IsDeformable != 0);

            if (mesh.IsSkinned) {
                mesh.SkinnedVertices.resize(meshHeader.VertexCount);
                in.read((char*)mesh.SkinnedVertices.data(), meshHeader.VertexCount * sizeof(SkinnedVertex));
                for (const auto& v : mesh.SkinnedVertices)
                    mesh.BindPoseAABB.Expand(v.Position);
            } else {
                mesh.Vertices.resize(meshHeader.VertexCount);
                in.read((char*)mesh.Vertices.data(), meshHeader.VertexCount * sizeof(Vertex));
                for (const auto& v : mesh.Vertices)
                    mesh.BindPoseAABB.Expand(v.Position);
            }

            mesh.Indices.resize(meshHeader.IndexCount);
            in.read((char*)mesh.Indices.data(), meshHeader.IndexCount * sizeof(u32));
        }

        // Read Skeleton
        if (modelHeader.BoneCount > 0) {
            ReadSkeleton(in, outData.SkeletonData, modelHeader.BoneCount);
        }

        // V3: read clip UUIDs
        if (modelHeader.AnimationCount > 0) {
            outData.AnimationClipUUIDs.resize(modelHeader.AnimationCount);
            in.read((char*)outData.AnimationClipUUIDs.data(),
                modelHeader.AnimationCount * sizeof(UUID));
        }

        // V4: scene graph (nodes + cameras + lights)
        ReadSceneGraph(in, outData, modelHeader.NodeCount, modelHeader.CameraCount, modelHeader.LightCount);

        return true;
    }

    bool AssetSerializer::SerializeMaterial(const fs::path& path, const MaterialAssetData& data)
    {
        LH_PROFILE_FUNCTION();
        return SerializeAtomic(path, [&](std::ofstream& out) {
            AssetHeader header;
            header.Type = AssetType::Material;
            out.write((char*)&header, sizeof(AssetHeader));

            std::string jsonStr = data.JsonData.dump();
            u32 size = (u32)jsonStr.size();
            out.write((char*)&size, sizeof(u32));
            out.write(jsonStr.data(), size);

            return true;
        });
    }

    bool AssetSerializer::DeserializeMaterial(const fs::path& path, MaterialAssetData& outData)
    {
        LH_PROFILE_FUNCTION();
        std::ifstream in(path, std::ios::binary);
        if (!in.is_open()) return false;

        AssetHeader header;
        in.read((char*)&header, sizeof(AssetHeader));
        if (header.Type != AssetType::Material) return false;

        u32 size;
        in.read((char*)&size, sizeof(u32));
        std::string jsonStr(size, '\0');
        in.read(jsonStr.data(), size);

        outData.JsonData = nlohmann::json::parse(jsonStr);
        return true;
    }

    bool AssetSerializer::SerializePhysicsMaterial(const fs::path& path, const PhysicsMaterialAssetData& data)
    {
        LH_PROFILE_FUNCTION();
        return SerializeAtomic(path, [&](std::ofstream& out) {
            AssetHeader header;
            header.Type = AssetType::PhysicsMaterial;
            out.write((char*)&header, sizeof(AssetHeader));

            std::string jsonStr = data.JsonData.dump();
            u32 size = (u32)jsonStr.size();
            out.write((char*)&size, sizeof(u32));
            out.write(jsonStr.data(), size);

            return true;
        });
    }

    bool AssetSerializer::DeserializePhysicsMaterial(const fs::path& path, PhysicsMaterialAssetData& outData)
    {
        LH_PROFILE_FUNCTION();
        std::ifstream in(path, std::ios::binary);
        if (!in.is_open()) return false;

        AssetHeader header;
        in.read((char*)&header, sizeof(AssetHeader));
        if (header.Type != AssetType::PhysicsMaterial) return false;

        u32 size;
        in.read((char*)&size, sizeof(u32));
        std::string jsonStr(size, '\0');
        in.read(jsonStr.data(), size);

        outData.JsonData = nlohmann::json::parse(jsonStr);
        return true;
    }

    bool AssetSerializer::SerializeShader(const fs::path& path, const ShaderAssetData& data)
    {
        LH_PROFILE_FUNCTION();
        return SerializeAtomic(path, [&](std::ofstream& out) {
            AssetHeader header;
            header.Version = 2; // V2: single-stage shader asset
            header.Type = AssetType::Shader;
            out.write((char*)&header, sizeof(AssetHeader));

            ShaderHeader shaderHeader;
            shaderHeader.Stage = (u32)data.Stage;
            shaderHeader.SpirVSize = (u32)data.SpirV.size();
            out.write((char*)&shaderHeader, sizeof(ShaderHeader));

            out.write((char*)data.SpirV.data(), data.SpirV.size() * sizeof(u32));

            return true;
        });
    }

    bool AssetSerializer::DeserializeShader(const fs::path& path, ShaderAssetData& outData)
    {
        LH_PROFILE_FUNCTION();
        std::ifstream in(path, std::ios::binary);
        if (!in.is_open()) return false;

        AssetHeader header;
        in.read((char*)&header, sizeof(AssetHeader));
        if (header.Type != AssetType::Shader) return false;

        // V2 schema: single-stage shader. V1 artifacts (paired vert+frag) are rejected so they get re-imported
        // under the new schema on first load.
        if (header.Version != 2) return false;

        ShaderHeader shaderHeader;
        in.read((char*)&shaderHeader, sizeof(ShaderHeader));

        outData.Stage = (ShaderStage)shaderHeader.Stage;
        outData.SpirV.resize(shaderHeader.SpirVSize);
        in.read((char*)outData.SpirV.data(), shaderHeader.SpirVSize * sizeof(u32));

        return true;
    }

    bool AssetSerializer::SerializeAnimation(const fs::path& path, const AnimationAssetData& data)
    {
        LH_PROFILE_FUNCTION();
        return SerializeAtomic(path, [&](std::ofstream& out) {
            AssetHeader header;
            header.Version = 1;
            header.Type = AssetType::Animation;
            out.write((const char*)&header, sizeof(AssetHeader));

            WriteAnimationClip(out, data.Clip);
            return true;
        });
    }

    bool AssetSerializer::DeserializeAnimation(const fs::path& path, AnimationAssetData& outData)
    {
        LH_PROFILE_FUNCTION();
        std::ifstream in(path, std::ios::binary);
        if (!in.is_open()) return false;

        AssetHeader header;
        in.read((char*)&header, sizeof(AssetHeader));
        if (header.Type != AssetType::Animation) return false;
        if (header.Version != 1) return false;

        ReadAnimationClip(in, outData.Clip);
        return true;
    }
}
