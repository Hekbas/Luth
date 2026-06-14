#include "luthpch.h"
#include "TextureImporter.h"
#include "luth/resources/AssetSerializer.h"
#include "luth/resources/Image.h"
#include "luth/resources/MetaFile.h"

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
            if (ts.contains("role"))             settings.Role = (TextureRole)ts["role"].get<int>();
        }

        Image::LoadResult8 img = Image::Load(source);
        if (!img.valid) {
            LH_CORE_ERROR("TextureImporter: Failed to load image {0}", source.string());
            return false;
        }

        // Canonicalize channels before serialize so the bindless artifact matches the shader's swizzles.
        ApplyRoleTransform(settings.Role, img.pixels);

        TextureAssetData texData;
        texData.Width    = static_cast<int>(img.width);
        texData.Height   = static_cast<int>(img.height);
        texData.Format   = TextureFormat::RGBA8;
        texData.Settings = settings;
        texData.Pixels   = std::move(img.pixels);

        return AssetSerializer::SerializeTexture(destination, texData);
    }
}