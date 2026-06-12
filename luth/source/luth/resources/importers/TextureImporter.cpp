#include "luthpch.h"
#include "TextureImporter.h"
#include "luth/resources/AssetSerializer.h"
#include "luth/resources/Image.h"
#include "luth/resources/MetaFile.h"

namespace Luth
{
    bool TextureImporter::Import(const std::filesystem::path& source, const std::filesystem::path& destination)
    {
        LH_PROFILE_FUNCTION();

        // Read import settings from .meta file
        TextureSettings settings;
        bool srgb = true; // color default; the model importer tags data maps (normal/MR/AO) srgb=false
        fs::path metaPath = source.string() + ".meta";
        MetaFile meta(UUID{});
        if (meta.Load(metaPath))
        {
            auto& ts = meta.GetTypeSettings();
            if (ts.contains("generate_mipmaps")) settings.GenerateMipmaps = ts["generate_mipmaps"].get<bool>();
            if (ts.contains("wrap_mode"))        settings.WrapMode = (TextureWrapMode)ts["wrap_mode"].get<int>();
            if (ts.contains("filter_min"))       settings.MinFilter = (TextureFilterMode)ts["filter_min"].get<int>();
            if (ts.contains("filter_mag"))       settings.MagFilter = (TextureFilterMode)ts["filter_mag"].get<int>();
            if (ts.contains("srgb"))             srgb = ts["srgb"].get<bool>();
        }

        Image::LoadResult8 img = Image::Load(source);
        if (!img.valid) {
            LH_CORE_ERROR("TextureImporter: Failed to load image {0}", source.string());
            return false;
        }

        TextureAssetData texData;
        texData.Width    = static_cast<int>(img.width);
        texData.Height   = static_cast<int>(img.height);
        texData.Format   = srgb ? TextureFormat::RGBA8_SRGB : TextureFormat::RGBA8;
        texData.Settings = settings;
        texData.Pixels   = std::move(img.pixels);

        return AssetSerializer::SerializeTexture(destination, texData);
    }
}