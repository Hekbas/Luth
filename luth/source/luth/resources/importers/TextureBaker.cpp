#include "luthpch.h"
#include "luth/resources/importers/TextureBaker.h"
#include "luth/resources/Image.h"
#include "luth/resources/MetaFile.h"
#include "luth/resources/AssetDatabase.h"
#include "luth/renderer/resources/Texture.h"

#include <algorithm>

namespace Luth::TextureBaker
{
    // Resize an RGBA8 buffer to (w,h) when it differs; the combine below needs matched dimensions.
    static std::vector<u8> FitTo(const std::vector<u8>& src, u32 sw, u32 sh, u32 w, u32 h)
    {
        if (sw == w && sh == h) return src;
        std::vector<u8> dst(static_cast<size_t>(w) * h * 4u);
        Image::Resize(src.data(), sw, sh, dst.data(), w, h, 4u);
        return dst;
    }

    // Pin role + dependencies on a freshly written baked PNG, register it, and drop any stale artifact so
    // the next load reimports the new pixels. role is LinearData so TextureImporter leaves the bytes alone.
    static UUID RegisterBaked(const fs::path& outPath, const std::vector<UUID>& deps)
    {
        UUID uuid = AssetDatabase::GetUUID(outPath);
        if (!uuid.IsValid()) uuid = MetaFile::Create(outPath, AssetType::Texture);

        const fs::path metaPath = outPath.string() + ".meta";
        MetaFile meta(uuid);
        meta.Load(metaPath);
        meta.GetTypeSettings()["role"] = (int)TextureRole::LinearData;
        for (const UUID& d : deps)
            if (d.IsValid()) meta.AddDependency(d);
        meta.Save(metaPath);

        AssetDatabase::RegisterAsset(outPath, uuid, AssetType::Texture);

        const fs::path artifact = AssetDatabase::GetArtifactPath(uuid);
        if (fs::exists(artifact)) fs::remove(artifact);
        return uuid;
    }

    UUID BakeMetalRough(const fs::path& outDir, const std::string& baseName,
                        const fs::path& roughnessSrc, const UUID& roughnessUuid,
                        const fs::path& metalnessSrc, const UUID& metalnessUuid)
    {
        Image::LoadResult8 rough = Image::Load(roughnessSrc);
        Image::LoadResult8 metal = Image::Load(metalnessSrc);
        if (!rough.valid || !metal.valid) {
            LH_CORE_WARN("TextureBaker: failed to load metal/rough inputs for '{0}'", baseName);
            return UUID::Invalid();
        }

        const u32 w = std::max(rough.width, metal.width);
        const u32 h = std::max(rough.height, metal.height);
        const std::vector<u8> r = FitTo(rough.pixels, rough.width, rough.height, w, h);
        const std::vector<u8> m = FitTo(metal.pixels, metal.width, metal.height, w, h);

        // Canonical metalRough: R is unused by the decode (occlusion is its own slot), G = roughness,
        // B = metallic, A opaque. Read each source's R (grayscale single-channel convention).
        std::vector<u8> out(static_cast<size_t>(w) * h * 4u);
        for (size_t px = 0; px + 3 < out.size(); px += 4) {
            out[px + 0] = 255u;
            out[px + 1] = r[px];
            out[px + 2] = m[px];
            out[px + 3] = 255u;
        }

        if (!fs::exists(outDir)) fs::create_directories(outDir);
        const fs::path outPath = outDir / (baseName + "_metalRough_baked.png");
        if (!Image::SavePng(outPath, out.data(), w, h, 4u)) {
            LH_CORE_WARN("TextureBaker: failed to write '{0}'", outPath.string());
            return UUID::Invalid();
        }

        LH_CORE_INFO("TextureBaker: packed metalRough '{0}' ({1}x{2})", outPath.filename().string(), w, h);
        return RegisterBaked(outPath, { roughnessUuid, metalnessUuid });
    }
}
