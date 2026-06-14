#pragma once

#include "luth/core/UUID.h"

#include <filesystem>

namespace Luth
{
    namespace fs = std::filesystem;

    // Synthesizes canonical textures from non-standard source layouts at import time, so material.slang's
    // fixed-swizzle decode always reads correct channels with no GPU or shader change. A baked texture is a
    // real PNG written next to the model and registered as an ordinary Texture asset (role LinearData),
    // reusing the embedded-texture-extraction path. Runs on a worker fiber (no Vulkan).
    namespace TextureBaker
    {
        // Pack separate single-channel roughness + metalness maps into one metalRough map (G = roughness,
        // B = metallic) matching the decode. Records both source UUIDs as dependencies of the baked asset.
        // Returns the baked texture UUID, or an invalid UUID on failure (caller floors to scalar factors).
        UUID BakeMetalRough(const fs::path& outDir, const std::string& baseName,
                            const fs::path& roughnessSrc, const UUID& roughnessUuid,
                            const fs::path& metalnessSrc, const UUID& metalnessUuid);

        // Inputs for a specular-glossiness to metal-rough conversion. The specular(-glossiness) map is
        // required (RGB = specular color, A = glossiness); diffuse may be a texture or just a factor.
        struct SpecGlossInputs
        {
            fs::path diffuseSrc;   UUID diffuseUuid;     // optional: empty path -> use diffuseFactor only
            fs::path specGlossSrc; UUID specGlossUuid;   // required
            float diffuseFactor[4]  = { 1.0f, 1.0f, 1.0f, 1.0f };
            float specularFactor[3] = { 1.0f, 1.0f, 1.0f };
            float glossinessFactor  = 1.0f;
        };

        struct SpecGlossResult { UUID baseColor = UUID::Invalid(); UUID metalRough = UUID::Invalid(); };

        // Convert specular-glossiness to metal-rough via the Khronos metallic-solve (glTF appendix): writes
        // a baseColor map and a metalRough map (G = roughness = 1 - gloss, B = solved metallic), folding all
        // factors in so the material's scalar color stays neutral. Lossy by nature; invalid UUIDs on failure.
        SpecGlossResult BakeSpecGlossToMetalRough(const fs::path& outDir, const std::string& baseName,
                                                  const SpecGlossInputs& in);
    }
}
