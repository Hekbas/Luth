#include "luthpch.h"
#include "TextureImporter.h"
#include "luth/resources/AssetSerializer.h"
#include "luth/resources/MetaFile.h"
#include <stb/stb_image.h>

namespace Luth
{
    bool TextureImporter::Import(const std::filesystem::path& source, const std::filesystem::path& destination)
    {
        LH_PROFILE_FUNCTION();

        // Read import settings from .meta file
        TextureSettings settings;
        fs::path metaPath = source.string() + ".meta";
        MetaFile meta(UUID{});
        if (meta.Load(metaPath))
        {
            auto& ts = meta.GetTypeSettings();
            if (ts.contains("generate_mipmaps")) settings.GenerateMipmaps = ts["generate_mipmaps"].get<bool>();
            if (ts.contains("wrap_mode"))        settings.WrapMode = (TextureWrapMode)ts["wrap_mode"].get<int>();
            if (ts.contains("filter_min"))       settings.MinFilter = (TextureFilterMode)ts["filter_min"].get<int>();
            if (ts.contains("filter_mag"))       settings.MagFilter = (TextureFilterMode)ts["filter_mag"].get<int>();
        }

        int width, height, channels;
        stbi_set_flip_vertically_on_load(1);

        // Force 4 channels for now (RGBA)
        stbi_uc* data = stbi_load(source.string().c_str(), &width, &height, &channels, 4);

        if (!data)
        {
            LH_CORE_ERROR("TextureImporter: Failed to load image {0}", source.string());
            return false;
        }

        TextureAssetData texData;
        texData.Width = width;
        texData.Height = height;
        texData.Format = TextureFormat::RGBA8;
        texData.Settings = settings;

        // Copy data to vector to own it
        size_t size = width * height * 4;
        texData.Pixels.resize(size);
        memcpy(texData.Pixels.data(), data, size);

        stbi_image_free(data);

        return AssetSerializer::SerializeTexture(destination, texData);
    }
}