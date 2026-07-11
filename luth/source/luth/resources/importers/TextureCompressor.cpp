#include "luthpch.h"
#include "luth/resources/importers/TextureCompressor.h"
#include "luth/resources/Image.h"
#include "luth/core/diagnostics/Log.h"

#include <bc7enc.h>
#include <rgbcx.h>

#include <algorithm>
#include <cmath>
#include <mutex>

namespace Luth
{
    namespace
    {
        std::once_flag g_encoderInit;
        void EnsureEncoderInit()
        {
            // Both inits are pure table setup; encode calls are read-only afterward (fiber/thread-safe).
            std::call_once(g_encoderInit, [] {
                rgbcx::init();
                bc7enc_compress_block_init();
            });
        }

        // Gather the 4x4 RGBA block whose top-left texel is (x0,y0) into dst[64]. Texels past the image
        // edge clamp to the last row/column so partial edge blocks never skew endpoint selection.
        void GatherBlock(const u8* img, u32 w, u32 h, u32 x0, u32 y0, u8 dst[64])
        {
            for (u32 ty = 0; ty < 4; ++ty)
            {
                u32 sy = std::min(y0 + ty, h - 1);
                for (u32 tx = 0; tx < 4; ++tx)
                {
                    u32 sx = std::min(x0 + tx, w - 1);
                    const u8* src = img + (sy * (u64)w + sx) * 4;
                    u8* d = dst + (ty * 4 + tx) * 4;
                    d[0] = src[0]; d[1] = src[1]; d[2] = src[2]; d[3] = src[3];
                }
            }
        }

        // Encode one tightly-packed RGBA8 mip level (w*h*4) and append its blocks to `out`.
        void EncodeLevel(const u8* img, u32 w, u32 h, TextureFormat fmt, TextureRole role,
                         CompressionQuality q, std::vector<u8>& out)
        {
            const TextureFormatInfo info = GetTextureFormatInfo(fmt);
            const u32 blocksX = (w + 3) / 4;
            const u32 blocksY = (h + 3) / 4;

            bc7enc_compress_block_params bc7p{};
            bc7enc_compress_block_params_init(&bc7p);
            // Color error weighted perceptually (YCbCr); data channels weighted uniformly.
            if (role == TextureRole::Color)
                bc7enc_compress_block_params_init_perceptual_weights(&bc7p);
            else
                bc7enc_compress_block_params_init_linear_weights(&bc7p);
            switch (q) {
                case CompressionQuality::Fast:   bc7p.m_uber_level = 0; bc7p.m_max_partitions = 16; break;
                case CompressionQuality::Normal: bc7p.m_uber_level = 1; bc7p.m_max_partitions = 64; break;
                case CompressionQuality::High:   bc7p.m_uber_level = 4; bc7p.m_max_partitions = 64; break;
            }
            const u32 bc1Level = (q == CompressionQuality::Fast)   ? 6u
                               : (q == CompressionQuality::Normal) ? 10u : 18u;

            const size_t base = out.size();
            out.resize(base + (size_t)blocksX * blocksY * info.blockBytes);
            u8* dst = out.data() + base;

            u8 block[64];
            for (u32 by = 0; by < blocksY; ++by)
            {
                for (u32 bx = 0; bx < blocksX; ++bx)
                {
                    GatherBlock(img, w, h, bx * 4, by * 4, block);
                    switch (fmt)
                    {
                        case TextureFormat::BC7_Unorm: bc7enc_compress_block(dst, block, &bc7p); break;
                        case TextureFormat::BC1_Unorm: rgbcx::encode_bc1(bc1Level, dst, block, true, false); break;
                        case TextureFormat::BC4_Unorm: rgbcx::encode_bc4(dst, block, 4); break;
                        case TextureFormat::BC5_Unorm: rgbcx::encode_bc5(dst, block, 0, 1, 4); break;
                        default: break;
                    }
                    dst += info.blockBytes;
                }
            }
        }
    }

    TextureFormat TextureCompressor::AutoFormatForRole(TextureRole role)
    {
        switch (role)
        {
            case TextureRole::NormalGL:
            case TextureRole::NormalDX: return TextureFormat::BC5_Unorm;
            default:                    return TextureFormat::BC7_Unorm; // Color / LinearData / GlossToRoughness
        }
    }

    CompressedTexture TextureCompressor::Compress(const u8* rgba, u32 width, u32 height, TextureFormat format,
                                                  TextureRole role, CompressionQuality quality, bool genMips)
    {
        CompressedTexture result;
        if (!GetTextureFormatInfo(format).compressed)
        {
            LH_LOG(Assets, error, "TextureCompressor: format {0} is not a BC format", (int)format);
            return result;
        }
        EnsureEncoderInit();

        result.format = format;
        result.width  = width;
        result.height = height;
        result.mipLevels = genMips
            ? (u32)std::floor(std::log2((f32)std::max(width, height))) + 1u
            : 1u;

        // Level 0 encodes the source; each later level halves the previous via Image::Resize (matches the
        // >> mip dimensions the runtime upload derives, so the concatenated blob lines up block-for-block).
        std::vector<u8> level(rgba, rgba + (size_t)width * height * 4);
        u32 mw = width, mh = height;
        for (u32 mip = 0; mip < result.mipLevels; ++mip)
        {
            if (mip > 0)
            {
                u32 nw = std::max(1u, mw >> 1);
                u32 nh = std::max(1u, mh >> 1);
                std::vector<u8> next((size_t)nw * nh * 4);
                Image::Resize(level.data(), mw, mh, next.data(), nw, nh, 4);
                level.swap(next);
                mw = nw; mh = nh;
            }
            EncodeLevel(level.data(), mw, mh, format, role, quality, result.data);
        }
        return result;
    }
}
