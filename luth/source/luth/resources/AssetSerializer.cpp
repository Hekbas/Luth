#include "luthpch.h"
#include "AssetSerializer.h"
#include "luth/resources/importers/TextureImporter.h"
#include "luth/resources/importers/ModelImporter.h"
#include "luth/resources/importers/MaterialImporter.h"
#include "luth/resources/importers/ShaderImporter.h"
#include <fstream>

namespace Luth
{
    bool AssetSerializer::SerializeTexture(const fs::path& path, const TextureAssetData& data)
    {
        std::ofstream out(path, std::ios::binary);
        if (!out.is_open()) return false;

        AssetHeader header;
        header.Type = AssetType::Texture;
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
        out.write((char*)&texHeader, sizeof(TextureHeader));

        out.write((char*)data.Pixels.data(), data.Pixels.size());
        return true;
    }

    bool AssetSerializer::DeserializeTexture(const fs::path& path, TextureAssetData& outData)
    {
        std::ifstream in(path, std::ios::binary);
        if (!in.is_open()) return false;

        AssetHeader header;
        in.read((char*)&header, sizeof(AssetHeader));
        if (header.Type != AssetType::Texture) return false;

        TextureHeader texHeader;
        in.read((char*)&texHeader, sizeof(TextureHeader));

        outData.Width = texHeader.Width;
        outData.Height = texHeader.Height;
        outData.Format = (TextureFormat)texHeader.Format;
        outData.Settings.GenerateMipmaps = texHeader.GenerateMipmaps != 0;
        outData.Settings.WrapMode = (TextureWrapMode)texHeader.WrapMode;
        outData.Settings.MinFilter = (TextureFilterMode)texHeader.MinFilter;
        outData.Settings.MagFilter = (TextureFilterMode)texHeader.MagFilter;
        outData.Pixels.resize(texHeader.SizeBytes);
        in.read((char*)outData.Pixels.data(), texHeader.SizeBytes);

        return true;
    }

    // --- Skeleton/Animation binary helpers ---

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

    static void WriteAnimationClips(std::ofstream& out, const std::vector<AnimationClip>& clips)
    {
        for (const auto& clip : clips) {
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

            // Events
            u32 eventCount = (u32)clip.Events.size();
            out.write((const char*)&eventCount, sizeof(u32));
            for (const auto& event : clip.Events) {
                out.write((const char*)&event.Time, sizeof(f32));
                WriteString(out, event.Name);
            }
        }
    }

    static void ReadAnimationClips(std::ifstream& in, std::vector<AnimationClip>& clips, u32 clipCount)
    {
        clips.resize(clipCount);
        for (u32 ci = 0; ci < clipCount; ++ci) {
            auto& clip = clips[ci];
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
    }

    // --- Model Serialization ---

    bool AssetSerializer::SerializeModel(const fs::path& path, const ModelAssetData& data)
    {
        std::ofstream out(path, std::ios::binary);
        if (!out.is_open()) return false;

        AssetHeader header;
        header.Version = 2; // V2: supports skeleton/animation data
        header.Type = AssetType::Model;
        out.write((const char*)&header, sizeof(AssetHeader));

        ModelHeader modelHeader;
        modelHeader.MeshCount = (u32)data.Meshes.size();
        modelHeader.MaterialCount = (u32)data.Materials.size();
        modelHeader.IsSkinned = data.IsSkinned ? 1 : 0;
        modelHeader.BoneCount = data.SkeletonData.BoneCount();
        modelHeader.AnimationCount = (u32)data.AnimationClips.size();
        out.write((const char*)&modelHeader, sizeof(ModelHeader));

        // Write Materials
        out.write((const char*)data.Materials.data(), data.Materials.size() * sizeof(UUID));

        // Write Meshes
        for (const auto& mesh : data.Meshes)
        {
            WriteString(out, mesh.Name);

            MeshHeader meshHeader;
            meshHeader.IsSkinned = mesh.IsSkinned ? 1 : 0;
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

        // Write Skeleton (V2)
        if (modelHeader.BoneCount > 0) {
            WriteSkeleton(out, data.SkeletonData);
        }

        // Write Animation Clips (V2)
        if (modelHeader.AnimationCount > 0) {
            WriteAnimationClips(out, data.AnimationClips);
        }

        return true;
    }

    bool AssetSerializer::DeserializeModel(const fs::path& path, ModelAssetData& outData)
    {
        std::ifstream in(path, std::ios::binary);
        if (!in.is_open()) return false;

        AssetHeader header;
        in.read((char*)&header, sizeof(AssetHeader));
        if (header.Type != AssetType::Model) return false;

        bool isV2 = (header.Version >= 2);

        if (isV2) {
            // V2 format with full ModelHeader
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

                if (mesh.IsSkinned) {
                    mesh.SkinnedVertices.resize(meshHeader.VertexCount);
                    in.read((char*)mesh.SkinnedVertices.data(), meshHeader.VertexCount * sizeof(SkinnedVertex));
                } else {
                    mesh.Vertices.resize(meshHeader.VertexCount);
                    in.read((char*)mesh.Vertices.data(), meshHeader.VertexCount * sizeof(Vertex));
                }

                mesh.Indices.resize(meshHeader.IndexCount);
                in.read((char*)mesh.Indices.data(), meshHeader.IndexCount * sizeof(u32));
            }

            // Read Skeleton
            if (modelHeader.BoneCount > 0) {
                ReadSkeleton(in, outData.SkeletonData, modelHeader.BoneCount);
            }

            // Read Animation Clips
            if (modelHeader.AnimationCount > 0) {
                ReadAnimationClips(in, outData.AnimationClips, modelHeader.AnimationCount);
            }
        }
        else {
            // V1 backward compatibility: old format with smaller ModelHeader
            // V1 ModelHeader only had MeshCount + MaterialCount (8 bytes)
            // We already read the full new ModelHeader, so we need to re-read
            in.seekg(sizeof(AssetHeader), std::ios::beg);

            u32 meshCount, materialCount;
            in.read((char*)&meshCount, sizeof(u32));
            in.read((char*)&materialCount, sizeof(u32));

            outData.IsSkinned = false;

            outData.Materials.resize(materialCount);
            in.read((char*)outData.Materials.data(), materialCount * sizeof(UUID));

            outData.Meshes.resize(meshCount);
            for (u32 i = 0; i < meshCount; i++)
            {
                auto& mesh = outData.Meshes[i];
                mesh.IsSkinned = false;

                u32 nameLen;
                in.read((char*)&nameLen, sizeof(u32));
                mesh.Name.resize(nameLen);
                in.read(mesh.Name.data(), nameLen);

                // V1 MeshHeader only had VertexCount + IndexCount + MaterialIndex (12 bytes)
                u32 vertexCount, indexCount, materialIndex;
                in.read((char*)&vertexCount, sizeof(u32));
                in.read((char*)&indexCount, sizeof(u32));
                in.read((char*)&materialIndex, sizeof(u32));
                mesh.MaterialIndex = materialIndex;

                mesh.Vertices.resize(vertexCount);
                in.read((char*)mesh.Vertices.data(), vertexCount * sizeof(Vertex));

                mesh.Indices.resize(indexCount);
                in.read((char*)mesh.Indices.data(), indexCount * sizeof(u32));
            }
        }

        return true;
    }

    bool AssetSerializer::SerializeMaterial(const fs::path& path, const MaterialAssetData& data)
    {
        std::ofstream out(path, std::ios::binary);
        if (!out.is_open()) return false;

        AssetHeader header;
        header.Type = AssetType::Material;
        out.write((char*)&header, sizeof(AssetHeader));

        std::string jsonStr = data.JsonData.dump();
        u32 size = (u32)jsonStr.size();
        out.write((char*)&size, sizeof(u32));
        out.write(jsonStr.data(), size);

        return true;
    }

    bool AssetSerializer::DeserializeMaterial(const fs::path& path, MaterialAssetData& outData)
    {
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

    bool AssetSerializer::SerializeShader(const fs::path& path, const ShaderAssetData& data)
    {
        std::ofstream out(path, std::ios::binary);
        if (!out.is_open()) return false;

        AssetHeader header;
        header.Type = AssetType::Shader;
        out.write((char*)&header, sizeof(AssetHeader));

        ShaderHeader shaderHeader;
        shaderHeader.VertexSpirVSize = (u32)data.VertexSpirV.size();
        shaderHeader.FragmentSpirVSize = (u32)data.FragmentSpirV.size();
        out.write((char*)&shaderHeader, sizeof(ShaderHeader));

        out.write((char*)data.VertexSpirV.data(), data.VertexSpirV.size() * sizeof(u32));
        out.write((char*)data.FragmentSpirV.data(), data.FragmentSpirV.size() * sizeof(u32));

        return true;
    }

    bool AssetSerializer::DeserializeShader(const fs::path& path, ShaderAssetData& outData)
    {
        std::ifstream in(path, std::ios::binary);
        if (!in.is_open()) return false;

        AssetHeader header;
        in.read((char*)&header, sizeof(AssetHeader));
        if (header.Type != AssetType::Shader) return false;

        ShaderHeader shaderHeader;
        in.read((char*)&shaderHeader, sizeof(ShaderHeader));

        outData.VertexSpirV.resize(shaderHeader.VertexSpirVSize);
        in.read((char*)outData.VertexSpirV.data(), shaderHeader.VertexSpirVSize * sizeof(u32));

        outData.FragmentSpirV.resize(shaderHeader.FragmentSpirVSize);
        in.read((char*)outData.FragmentSpirV.data(), shaderHeader.FragmentSpirVSize * sizeof(u32));

        return true;
    }
}
