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
        outData.Pixels.resize(texHeader.SizeBytes);
        in.read((char*)outData.Pixels.data(), texHeader.SizeBytes);

        return true;
    }

    bool AssetSerializer::SerializeModel(const fs::path& path, const ModelAssetData& data)
    {
        std::ofstream out(path, std::ios::binary);
        if (!out.is_open()) return false;

        AssetHeader header;
        header.Type = AssetType::Model;
        out.write((char*)&header, sizeof(AssetHeader));

        ModelHeader modelHeader;
        modelHeader.MeshCount = (u32)data.Meshes.size();
        modelHeader.MaterialCount = (u32)data.Materials.size();
        out.write((char*)&modelHeader, sizeof(ModelHeader));

        // Write Materials
        out.write((char*)data.Materials.data(), data.Materials.size() * sizeof(UUID));

        // Write Meshes
        for (const auto& mesh : data.Meshes)
        {
            MeshHeader meshHeader;
            meshHeader.VertexCount = (u32)mesh.Vertices.size();
            meshHeader.IndexCount = (u32)mesh.Indices.size();
            meshHeader.MaterialIndex = mesh.MaterialIndex;
            
            // Write Name (Fixed size or length prefixed? Let's use length prefix)
            u32 nameLen = (u32)mesh.Name.size();
            out.write((char*)&nameLen, sizeof(u32));
            out.write(mesh.Name.data(), nameLen);

            out.write((char*)&meshHeader, sizeof(MeshHeader));
            out.write((char*)mesh.Vertices.data(), mesh.Vertices.size() * sizeof(Vertex));
            out.write((char*)mesh.Indices.data(), mesh.Indices.size() * sizeof(u32));
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

        ModelHeader modelHeader;
        in.read((char*)&modelHeader, sizeof(ModelHeader));

        // Read Materials
        outData.Materials.resize(modelHeader.MaterialCount);
        in.read((char*)outData.Materials.data(), modelHeader.MaterialCount * sizeof(UUID));

        // Read Meshes
        outData.Meshes.resize(modelHeader.MeshCount);
        for (u32 i = 0; i < modelHeader.MeshCount; i++)
        {
            auto& mesh = outData.Meshes[i];
            
            u32 nameLen;
            in.read((char*)&nameLen, sizeof(u32));
            mesh.Name.resize(nameLen);
            in.read(mesh.Name.data(), nameLen);

            MeshHeader meshHeader;
            in.read((char*)&meshHeader, sizeof(MeshHeader));
            mesh.MaterialIndex = meshHeader.MaterialIndex;

            mesh.Vertices.resize(meshHeader.VertexCount);
            in.read((char*)mesh.Vertices.data(), meshHeader.VertexCount * sizeof(Vertex));

            mesh.Indices.resize(meshHeader.IndexCount);
            in.read((char*)mesh.Indices.data(), meshHeader.IndexCount * sizeof(u32));
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
