#include "luthpch.h"
#include "luth/resources/importers/TextureBaker.h"
#include "luth/resources/Image.h"
#include "luth/resources/MetaFile.h"
#include "luth/resources/AssetDatabase.h"
#include "luth/renderer/resources/Texture.h"

#include <algorithm>
#include <cmath>

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
    // the next load reimports the new pixels. A baked map is already canonical, so its role suppresses any
    // further import transform (LinearData for data maps, Color for the converted baseColor).
    static UUID RegisterBaked(const fs::path& outPath, TextureRole role, const std::vector<UUID>& deps)
    {
        LH_PROFILE_FUNCTION();
        UUID uuid = AssetDatabase::GetUUID(outPath);
        if (!uuid.IsValid()) uuid = MetaFile::Create(outPath, AssetType::Texture);

        const fs::path metaPath = outPath.string() + ".meta";
        MetaFile meta(uuid);
        meta.Load(metaPath);
        meta.GetTypeSettings()["role"] = (int)role;
        for (const UUID& d : deps)
            if (d.IsValid()) meta.AddDependency(d);
        meta.Save(metaPath);

        AssetDatabase::RegisterAsset(outPath, uuid, AssetType::Texture);

        const fs::path artifact = AssetDatabase::GetArtifactPath(uuid);
        if (fs::exists(artifact)) fs::remove(artifact);
        return uuid;
    }

    static float Luma(float r, float g, float b) { return 0.2126f * r + 0.7152f * g + 0.0722f * b; }

    // Khronos specular-glossiness -> metallic solve (glTF appendix). Inputs are linear 0..1 brightnesses.
    static float SolveMetallic(float diffuseLum, float specLum, float oneMinusSpecStrength)
    {
        const float kDielectric = 0.04f;
        if (specLum < kDielectric) return 0.0f;
        const float a = kDielectric;
        const float b = diffuseLum * oneMinusSpecStrength / (1.0f - kDielectric) + specLum - 2.0f * kDielectric;
        const float c = kDielectric - specLum;
        const float disc = std::max(b * b - 4.0f * a * c, 0.0f);
        return std::clamp((-b + std::sqrt(disc)) / (2.0f * a), 0.0f, 1.0f);
    }

    static u8 ToByte(float v) { return static_cast<u8>(std::lround(std::clamp(v, 0.0f, 1.0f) * 255.0f)); }

    UUID BakeMetalRough(const fs::path& outDir, const std::string& baseName,
                        const fs::path& roughnessSrc, const UUID& roughnessUuid,
                        const fs::path& metalnessSrc, const UUID& metalnessUuid)
    {
        LH_PROFILE_FUNCTION();
        Image::LoadResult8 rough = Image::Load(roughnessSrc);
        Image::LoadResult8 metal = Image::Load(metalnessSrc);
        if (!rough.valid || !metal.valid) {
            LH_LOG(Assets, warn, "TextureBaker: failed to load metal/rough inputs for '{0}'", baseName);
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
            LH_LOG(Assets, warn, "TextureBaker: failed to write '{0}'", outPath.string());
            return UUID::Invalid();
        }

        LH_LOG(Assets, info, "TextureBaker: packed metalRough '{0}' ({1}x{2})", outPath.filename().string(), w, h);
        return RegisterBaked(outPath, TextureRole::LinearData, { roughnessUuid, metalnessUuid });
    }

    SpecGlossResult BakeSpecGlossToMetalRough(const fs::path& outDir, const std::string& baseName,
                                              const SpecGlossInputs& in)
    {
        LH_PROFILE_FUNCTION();
        SpecGlossResult result;
        Image::LoadResult8 sg = Image::Load(in.specGlossSrc);
        if (!sg.valid) {
            LH_LOG(Assets, warn, "TextureBaker: failed to load spec-gloss input for '{0}'", baseName);
            return result;
        }
        const u32 w = sg.width, h = sg.height;

        // Diffuse: a texture resized to the spec-gloss size, or a flat factor color when absent.
        std::vector<u8> diff;
        const bool hasDiffTex = !in.diffuseSrc.empty() && [&] {
            Image::LoadResult8 d = Image::Load(in.diffuseSrc);
            if (!d.valid) return false;
            diff = FitTo(d.pixels, d.width, d.height, w, h);
            return true;
        }();

        std::vector<u8> baseOut(static_cast<size_t>(w) * h * 4u);
        std::vector<u8> mrOut(static_cast<size_t>(w) * h * 4u);
        const float eps = 1e-4f;

        for (size_t px = 0; px + 3 < sg.pixels.size(); px += 4) {
            const float sr = (sg.pixels[px + 0] / 255.0f) * in.specularFactor[0];
            const float sgc = (sg.pixels[px + 1] / 255.0f) * in.specularFactor[1];
            const float sb = (sg.pixels[px + 2] / 255.0f) * in.specularFactor[2];
            const float gloss = (sg.pixels[px + 3] / 255.0f) * in.glossinessFactor;

            float dr = in.diffuseFactor[0], dg = in.diffuseFactor[1], db = in.diffuseFactor[2], da = in.diffuseFactor[3];
            if (hasDiffTex) {
                dr *= diff[px + 0] / 255.0f; dg *= diff[px + 1] / 255.0f;
                db *= diff[px + 2] / 255.0f; da *= diff[px + 3] / 255.0f;
            }

            const float oneMinusSpec = 1.0f - std::max(sr, std::max(sgc, sb));
            const float metallic = SolveMetallic(Luma(dr, dg, db), Luma(sr, sgc, sb), oneMinusSpec);

            auto fromDiffuse = [&](float d) { return d * oneMinusSpec / (1.0f - 0.04f) / std::max(1.0f - metallic, eps); };
            auto fromSpec    = [&](float s) { return (s - 0.04f * (1.0f - metallic)) / std::max(metallic, eps); };
            const float t = metallic * metallic;

            baseOut[px + 0] = ToByte(fromDiffuse(dr) * (1.0f - t) + fromSpec(sr) * t);
            baseOut[px + 1] = ToByte(fromDiffuse(dg) * (1.0f - t) + fromSpec(sgc) * t);
            baseOut[px + 2] = ToByte(fromDiffuse(db) * (1.0f - t) + fromSpec(sb) * t);
            baseOut[px + 3] = ToByte(da);

            mrOut[px + 0] = 255u;
            mrOut[px + 1] = ToByte(1.0f - gloss);   // roughness -> G
            mrOut[px + 2] = ToByte(metallic);       // metallic  -> B
            mrOut[px + 3] = 255u;
        }

        if (!fs::exists(outDir)) fs::create_directories(outDir);
        const fs::path basePath = outDir / (baseName + "_baseColor_baked.png");
        const fs::path mrPath   = outDir / (baseName + "_metalRough_baked.png");
        if (!Image::SavePng(basePath, baseOut.data(), w, h, 4u) ||
            !Image::SavePng(mrPath, mrOut.data(), w, h, 4u)) {
            LH_LOG(Assets, warn, "TextureBaker: failed to write spec-gloss outputs for '{0}'", baseName);
            return result;
        }

        const std::vector<UUID> deps = { in.specGlossUuid, in.diffuseUuid };
        result.baseColor  = RegisterBaked(basePath, TextureRole::Color, deps);
        result.metalRough = RegisterBaked(mrPath, TextureRole::LinearData, deps);
        LH_LOG(Assets, info, "TextureBaker: converted spec-gloss '{0}' ({1}x{2})", baseName, w, h);
        return result;
    }
}
