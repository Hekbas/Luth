#include "luthpch.h"
#include "luth/resources/Image.h"
#include "luth/core/diagnostics/Log.h"

#define STB_IMAGE_IMPLEMENTATION
#include <stb/stb_image.h>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb/stb_image_write.h>

#define STB_IMAGE_RESIZE_IMPLEMENTATION
#include <stb/stb_image_resize.h>

#include <cstring>

namespace Luth::Image
{
    void Init()
    {
        // Set ONCE at engine boot. No other site is allowed to call
        // stbi_set_flip_vertically_on_load — engine-wide convention is
        // top-left origin. Bottom-left consumers post-process via
        // FlipVertical* after Load*.
        stbi_set_flip_vertically_on_load(0);
    }

    LoadResult8 Load(const fs::path& path)
    {
        LoadResult8 out;
        int w = 0, h = 0, c = 0;
        stbi_uc* data = stbi_load(path.string().c_str(), &w, &h, &c, 4);
        if (!data) return out;
        out.pixels.assign(data, data + static_cast<size_t>(w) * h * 4);
        stbi_image_free(data);
        out.width  = static_cast<u32>(w);
        out.height = static_cast<u32>(h);
        out.valid  = true;
        return out;
    }

    LoadResult8 LoadFromMemory(const void* data, size_t size)
    {
        LoadResult8 out;
        if (!data || size == 0) return out;
        int w = 0, h = 0, c = 0;
        stbi_uc* pixels = stbi_load_from_memory(
            static_cast<const stbi_uc*>(data),
            static_cast<int>(size), &w, &h, &c, 4);
        if (!pixels) return out;
        out.pixels.assign(pixels, pixels + static_cast<size_t>(w) * h * 4);
        stbi_image_free(pixels);
        out.width  = static_cast<u32>(w);
        out.height = static_cast<u32>(h);
        out.valid  = true;
        return out;
    }

    LoadResultF LoadHDR(const fs::path& path)
    {
        LoadResultF out;
        int w = 0, h = 0, c = 0;
        f32* data = stbi_loadf(path.string().c_str(), &w, &h, &c, 4);
        if (!data) return out;
        const size_t total = static_cast<size_t>(w) * h * 4;
        out.pixels.assign(data, data + total);
        stbi_image_free(data);
        out.width  = static_cast<u32>(w);
        out.height = static_cast<u32>(h);
        out.valid  = true;
        return out;
    }

    void FlipVertical(u8* pixels, u32 width, u32 height, u32 channels)
    {
        if (!pixels || width == 0 || height < 2 || channels == 0) return;
        const size_t row = static_cast<size_t>(width) * channels;
        std::vector<u8> tmp(row);
        for (u32 y = 0; y < height / 2; ++y) {
            u8* a = pixels + static_cast<size_t>(y) * row;
            u8* b = pixels + static_cast<size_t>(height - 1 - y) * row;
            std::memcpy(tmp.data(), a, row);
            std::memcpy(a, b, row);
            std::memcpy(b, tmp.data(), row);
        }
    }

    void FlipVerticalF32(f32* pixels, u32 width, u32 height, u32 channels)
    {
        if (!pixels || width == 0 || height < 2 || channels == 0) return;
        const size_t row    = static_cast<size_t>(width) * channels;
        const size_t rowBytes = row * sizeof(f32);
        std::vector<f32> tmp(row);
        for (u32 y = 0; y < height / 2; ++y) {
            f32* a = pixels + static_cast<size_t>(y) * row;
            f32* b = pixels + static_cast<size_t>(height - 1 - y) * row;
            std::memcpy(tmp.data(), a, rowBytes);
            std::memcpy(a, b, rowBytes);
            std::memcpy(b, tmp.data(), rowBytes);
        }
    }

    std::vector<u8> EncodePngToMemory(const u8* pixels, u32 w, u32 h, u32 channels)
    {
        std::vector<u8> out;
        if (!pixels || w == 0 || h == 0 || channels == 0) return out;
        auto writer = [](void* userCtx, void* data, int sz) {
            auto* dst = static_cast<std::vector<u8>*>(userCtx);
            const u8* p = static_cast<const u8*>(data);
            dst->insert(dst->end(), p, p + static_cast<size_t>(sz));
        };
        const int rc = stbi_write_png_to_func(
            writer, &out,
            static_cast<int>(w), static_cast<int>(h),
            static_cast<int>(channels), pixels,
            static_cast<int>(w * channels));
        if (!rc) out.clear();
        return out;
    }

    bool Resize(const u8* src, u32 srcW, u32 srcH,
                u8* dst, u32 dstW, u32 dstH, u32 channels)
    {
        if (!src || !dst || srcW == 0 || srcH == 0 || dstW == 0 || dstH == 0)
            return false;
        const int rc = stbir_resize_uint8(
            src, static_cast<int>(srcW), static_cast<int>(srcH), 0,
            dst, static_cast<int>(dstW), static_cast<int>(dstH), 0,
            static_cast<int>(channels));
        return rc != 0;
    }
}
