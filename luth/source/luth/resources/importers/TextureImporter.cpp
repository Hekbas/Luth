#include "luthpch.h"
#include "TextureImporter.h"
#include "luth/resources/AssetSerializer.h"
#include "luth/resources/Image.h"
#include "luth/resources/MetaFile.h"
#include "luth/resources/importers/TextureCompressor.h"

#include <algorithm>

namespace Luth
{
    // Rewrite RGBA8 bytes in place so material.slang's fixed swizzles read canonical channels. Runs on a
    // worker fiber (no Vulkan); mips are generated later on GPU upload from these canonical bytes, so the
    // per-texel affine ops here commute with the downstream box-downsample (correct at every mip).
    static void ApplyRoleTransform(TextureRole role, std::vector<u8>& px)
    {
        switch (role)
        {
            case TextureRole::NormalDX:
                // DirectX normals store -Y; flip green so the decode's *2-1 yields the +Y the BRDF wants.
                for (size_t i = 1; i < px.size(); i += 4)
                    px[i] = static_cast<u8>(255u - px[i]);
                break;
            case TextureRole::GlossToRoughness:
                // The roughness channel (G, glTF metalRough convention) holds perceptual gloss; invert it
                // so the decode's .g reads roughness. Metallic (.b) and the rest are left intact.
                for (size_t i = 1; i < px.size(); i += 4)
                    px[i] = static_cast<u8>(255u - px[i]);
                break;
            default:
                break; // Color / NormalGL / LinearData are already canonical
        }
    }

    // .meta "compression" string -> target format. "none" keeps RGBA8; "auto" resolves per role.
    static TextureFormat ResolveCompression(const std::string& mode, TextureRole role, bool& outNone)
    {
        outNone = (mode == "none");
        if (outNone)        return TextureFormat::RGBA8;
        if (mode == "bc1")  return TextureFormat::BC1_Unorm;
        if (mode == "bc4")  return TextureFormat::BC4_Unorm;
        if (mode == "bc5")  return TextureFormat::BC5_Unorm;
        if (mode == "bc7")  return TextureFormat::BC7_Unorm;
        return TextureCompressor::AutoFormatForRole(role); // "auto" / unknown
    }

    static bool HasSubOpaqueAlpha(const std::vector<u8>& rgba)
    {
        for (size_t i = 3; i < rgba.size(); i += 4)
            if (rgba[i] < 255) return true;
        return false;
    }

    bool TextureImporter::Import(const std::filesystem::path& source, const std::filesystem::path& destination)
    {
        LH_PROFILE_FUNCTION();

        // Read import settings from .meta file (legacy compression_format/srgb keys are ignored)
        TextureSettings settings;
        std::string compMode = "auto";
        int compQuality = 1;
        fs::path metaPath = source.string() + ".meta";
        MetaFile meta(UUID{});
        if (meta.Load(metaPath))
        {
            auto& ts = meta.GetTypeSettings();
            if (ts.contains("generate_mipmaps")) settings.GenerateMipmaps = ts["generate_mipmaps"].get<bool>();
            if (ts.contains("wrap_mode"))        settings.WrapMode = (TextureWrapMode)ts["wrap_mode"].get<int>();
            if (ts.contains("filter_min"))       settings.MinFilter = (TextureFilterMode)ts["filter_min"].get<int>();
            if (ts.contains("filter_mag"))       settings.MagFilter = (TextureFilterMode)ts["filter_mag"].get<int>();
            if (ts.contains("role"))             settings.Role = (TextureRole)ts["role"].get<int>();
            if (ts.contains("compression"))         compMode = ts["compression"].get<std::string>();
            if (ts.contains("compression_quality")) compQuality = ts["compression_quality"].get<int>();
        }

        Image::LoadResult8 img = Image::Load(source);
        if (!img.valid) {
            LH_LOG(Assets, error, "TextureImporter: Failed to load image {0}", source.string());
            return false;
        }

        // Canonicalize channels before compress/serialize so the artifact matches the shader swizzles;
        // the affine role ops commute with the box-downsample, so the mip chain stays correct.
        ApplyRoleTransform(settings.Role, img.pixels);

        TextureAssetData texData;
        texData.Width    = img.width;
        texData.Height   = img.height;
        texData.Settings = settings;

        bool none = false;
        TextureFormat fmt = ResolveCompression(compMode, settings.Role, none);
        if (none)
        {
            texData.Format    = TextureFormat::RGBA8;
            texData.MipLevels = 1; // runtime blit-generates from GenerateMipmaps
            texData.Pixels    = std::move(img.pixels);
        }
        else
        {
            if (fmt == TextureFormat::BC1_Unorm && HasSubOpaqueAlpha(img.pixels))
                LH_LOG(Assets, warn, "TextureImporter: BC1 override drops alpha on {0}", source.string());

            CompressionQuality q = (CompressionQuality)std::clamp(compQuality, 0, 2);
            CompressedTexture ct = TextureCompressor::Compress(img.pixels.data(), img.width, img.height,
                                                               fmt, settings.Role, q, settings.GenerateMipmaps);
            texData.Format    = ct.format;
            texData.MipLevels = ct.mipLevels;
            texData.Pixels    = std::move(ct.data);
        }

        return AssetSerializer::SerializeTexture(destination, texData);
    }
}