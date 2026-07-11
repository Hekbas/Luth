#pragma once

#include "luth/core/types/LuthTypes.h"
#include "luth/renderer/resources/Texture.h"

#include <vector>

namespace Luth
{
    enum class CompressionQuality : u8 { Fast = 0, Normal = 1, High = 2 };

    struct CompressedTexture
    {
        TextureFormat format = TextureFormat::None;
        u32 width = 0, height = 0;
        u32 mipLevels = 0;
        std::vector<u8> data; // concatenated levels, largest first (TextureLevelBytes layout)
    };

    // Import-time BCn block encoder: stb_image_resize mip chain + bc7enc (BC7) / rgbcx (BC1/4/5).
    // Pure CPU, safe on worker fibers; the one-time result is cached in the texture artifact.
    namespace TextureCompressor
    {
        // Role -> default BC format for the "auto" policy. Deliberately never returns BC1/BC4 -- BC1
        // drops alpha to 1 bit and BC4 samples (R,0,0,1) which would zero ORM's rough/metal; both are
        // explicit-override-only. Normals -> BC5, everything else -> BC7.
        TextureFormat AutoFormatForRole(TextureRole role);

        // Compress tightly-packed RGBA8 (width*height*4 bytes) to `format`, generating a full mip chain
        // unless genMips is false. `role` picks BC7 perceptual (Color) vs linear (data) weighting.
        // Returns an empty result (mipLevels == 0) if `format` is not a BC format.
        CompressedTexture Compress(const u8* rgba, u32 width, u32 height, TextureFormat format,
                                   TextureRole role, CompressionQuality quality, bool genMips);
    }
}
