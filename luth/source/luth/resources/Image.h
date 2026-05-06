#pragma once

#include "luth/core/types/LuthTypes.h"

#include <filesystem>
#include <vector>

namespace Luth::Image
{
    namespace fs = std::filesystem;

    // 8-bit-per-channel decoded image. RGBA8, top-left origin.
    struct LoadResult8
    {
        std::vector<u8> pixels;
        u32  width  = 0;
        u32  height = 0;
        bool valid  = false;
    };

    // 32-bit-float decoded image (HDR). RGBA32F, top-left origin.
    struct LoadResultF
    {
        std::vector<f32> pixels;
        u32  width  = 0;
        u32  height = 0;
        bool valid  = false;
    };

    // invariant: this is the ONLY site in the engine that touches stb's global
    // flip flag. Init() sets it to 0 once at App boot; every Image::Load* path
    // returns top-left origin (Vulkan + ImGui native). Callers needing
    // bottom-left (legacy OpenGL / IBL convention) call FlipVertical* after.
    void Init();

    // 8-bit RGBA loaders. Always 4 channels, always top-left origin.
    LoadResult8 Load(const fs::path& path);
    LoadResult8 LoadFromMemory(const void* data, size_t size);

    // HDR loader. Always 4 channels (RGBA32F), always top-left origin.
    LoadResultF LoadHDR(const fs::path& path);

    // Post-decode in-place vertical flip. Cheap row-swap memcpy.
    void FlipVertical(u8*  pixels, u32 width, u32 height, u32 channels);
    void FlipVerticalF32(f32* pixels, u32 width, u32 height, u32 channels);

    // PNG encode (in-memory) for ThumbnailGenerator's IO write path.
    std::vector<u8> EncodePngToMemory(const u8* pixels, u32 w, u32 h, u32 channels);

    // Resize wrapper for ThumbnailGenerator's downscale.
    bool Resize(const u8* src, u32 srcW, u32 srcH,
                u8* dst, u32 dstW, u32 dstH, u32 channels);
}
