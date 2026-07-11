// TextureCompressor -- BCn block encode + mip chain. Verifies the concatenated-level layout the runtime
// upload path relies on (largest first, whole 4x4 blocks) and that edge dims (NPOT, 1x1) encode safely.

#include <doctest/doctest.h>

#include "luth/resources/importers/TextureCompressor.h"
#include "luth/renderer/resources/Texture.h"

#include <vector>
#include <cmath>
#include <algorithm>

using namespace Luth;

namespace
{
    // Mirror the compressor + upload dimension derivation to predict the concatenated payload size.
    u64 ExpectedSize(u32 w, u32 h, TextureFormat fmt, u32 mipLevels)
    {
        u64 total = 0;
        u32 mw = w, mh = h;
        for (u32 mip = 0; mip < mipLevels; ++mip)
        {
            total += TextureLevelBytes(fmt, mw, mh);
            mw = std::max(1u, mw >> 1);
            mh = std::max(1u, mh >> 1);
        }
        return total;
    }

    u32 FullMipCount(u32 w, u32 h)
    {
        return (u32)std::floor(std::log2((f32)std::max(w, h))) + 1u;
    }

    std::vector<u8> SolidRGBA(u32 w, u32 h, u8 r, u8 g, u8 b, u8 a)
    {
        std::vector<u8> px((size_t)w * h * 4);
        for (size_t i = 0; i < px.size(); i += 4) { px[i]=r; px[i+1]=g; px[i+2]=b; px[i+3]=a; }
        return px;
    }
}

TEST_CASE("TextureLevelBytes: BC block sizes [smoke]")
{
    // BC7/BC5 = 16 B/block, BC1/BC4 = 8 B/block, 4x4 texels per block; sub-4x4 occupies one full block.
    CHECK(TextureLevelBytes(TextureFormat::BC7_Unorm, 4, 4) == 16);
    CHECK(TextureLevelBytes(TextureFormat::BC7_Unorm, 1, 1) == 16);
    CHECK(TextureLevelBytes(TextureFormat::BC7_Unorm, 8, 8) == 64);
    CHECK(TextureLevelBytes(TextureFormat::BC1_Unorm, 8, 8) == 32);
    CHECK(TextureLevelBytes(TextureFormat::BC4_Unorm, 4, 4) == 8);
    CHECK(TextureLevelBytes(TextureFormat::BC5_Unorm, 4, 4) == 16);
    // NPOT rounds up to whole 4x4 blocks: ceil(25/4) = 7 per dim.
    CHECK(TextureLevelBytes(TextureFormat::BC7_Unorm, 25, 25) == (u64)7 * 7 * 16);
    CHECK(TextureLevelBytes(TextureFormat::BC7_Unorm, 4096, 4096) == (u64)1024 * 1024 * 16);
    // Uncompressed reports bytes-per-texel (blockDim 1).
    CHECK(TextureLevelBytes(TextureFormat::RGBA8, 4, 4) == 64);
}

TEST_CASE("GetTextureFormatInfo: compressed flags [smoke]")
{
    auto bc7 = GetTextureFormatInfo(TextureFormat::BC7_Unorm);
    CHECK(bc7.compressed);
    CHECK(bc7.blockDim == 4);
    CHECK(bc7.blockBytes == 16);
    CHECK(GetTextureFormatInfo(TextureFormat::BC1_Unorm).blockBytes == 8);
    CHECK_FALSE(GetTextureFormatInfo(TextureFormat::RGBA8).compressed);
}

TEST_CASE("AutoFormatForRole: normals BC5, rest BC7 [smoke]")
{
    CHECK(TextureCompressor::AutoFormatForRole(TextureRole::NormalGL) == TextureFormat::BC5_Unorm);
    CHECK(TextureCompressor::AutoFormatForRole(TextureRole::NormalDX) == TextureFormat::BC5_Unorm);
    CHECK(TextureCompressor::AutoFormatForRole(TextureRole::Color) == TextureFormat::BC7_Unorm);
    CHECK(TextureCompressor::AutoFormatForRole(TextureRole::LinearData) == TextureFormat::BC7_Unorm);
    CHECK(TextureCompressor::AutoFormatForRole(TextureRole::GlossToRoughness) == TextureFormat::BC7_Unorm);
}

TEST_CASE("Compress: mip chain size matches upload layout [smoke]")
{
    auto px = SolidRGBA(8, 8, 200, 120, 60, 255);
    auto ct = TextureCompressor::Compress(px.data(), 8, 8, TextureFormat::BC7_Unorm,
                                          TextureRole::Color, CompressionQuality::Fast, true);
    CHECK(ct.format == TextureFormat::BC7_Unorm);
    CHECK(ct.mipLevels == FullMipCount(8, 8)); // 8,4,2,1 -> 4
    CHECK(ct.mipLevels == 4);
    CHECK(ct.data.size() == ExpectedSize(8, 8, TextureFormat::BC7_Unorm, ct.mipLevels));
    // Encoder actually wrote a non-degenerate block (endpoints + selectors, not zero-filled).
    CHECK(std::any_of(ct.data.begin(), ct.data.end(), [](u8 b){ return b != 0; }));
}

TEST_CASE("Compress: single level when genMips is false [smoke]")
{
    auto px = SolidRGBA(8, 8, 10, 20, 30, 255);
    auto ct = TextureCompressor::Compress(px.data(), 8, 8, TextureFormat::BC7_Unorm,
                                          TextureRole::Color, CompressionQuality::Fast, false);
    CHECK(ct.mipLevels == 1);
    CHECK(ct.data.size() == 64); // one 8x8 BC7 level (2x2 blocks * 16 B)
}

TEST_CASE("Compress: NPOT dimensions encode without overrun [smoke]")
{
    auto px = SolidRGBA(25, 25, 90, 90, 90, 255);
    auto ct = TextureCompressor::Compress(px.data(), 25, 25, TextureFormat::BC7_Unorm,
                                          TextureRole::LinearData, CompressionQuality::Fast, true);
    CHECK(ct.mipLevels == FullMipCount(25, 25)); // 25,12,6,3,1 -> 5
    CHECK(ct.data.size() == ExpectedSize(25, 25, TextureFormat::BC7_Unorm, ct.mipLevels));
}

TEST_CASE("Compress: 1x1 and BC5/BC1 edge cases [smoke]")
{
    auto px1 = SolidRGBA(1, 1, 128, 128, 255, 255);
    auto bc5 = TextureCompressor::Compress(px1.data(), 1, 1, TextureFormat::BC5_Unorm,
                                           TextureRole::NormalGL, CompressionQuality::Normal, true);
    CHECK(bc5.mipLevels == 1);
    CHECK(bc5.data.size() == 16); // one BC5 block

    auto px4 = SolidRGBA(4, 4, 200, 200, 200, 255);
    auto bc1 = TextureCompressor::Compress(px4.data(), 4, 4, TextureFormat::BC1_Unorm,
                                           TextureRole::Color, CompressionQuality::High, false);
    CHECK(bc1.data.size() == 8); // one BC1 block
}
