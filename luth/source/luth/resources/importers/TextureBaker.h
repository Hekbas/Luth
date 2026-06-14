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
    }
}
