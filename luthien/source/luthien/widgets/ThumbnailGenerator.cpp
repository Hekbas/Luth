#include "lepch.h"
#include "luthien/widgets/ThumbnailGenerator.h"
#include "luthien/widgets/ThumbnailCache.h"

#include "luth/core/diagnostics/Log.h"
#include "luth/jobs/IOThread.h"
#include "luth/jobs/JobSystem.h"
#include "luth/memory/MemoryMacros.h"
#include "luth/resources/AssetDatabase.h"
#include "luth/resources/FileSystem.h"

#include <stb/stb_image.h>
#include <stb/stb_image_resize.h>
#include <stb/stb_image_write.h>

#include <filesystem>
#include <vector>

namespace Luth::UI
{
    namespace fs = std::filesystem;

    // ThumbnailCache exposes these to its sibling generator only — wire-internal.
    namespace ThumbnailCacheInternal
    {
        void PushTextureCompletion(UUID asset, std::vector<u8> pixels, u32 width, u32 height);
        void NotifyBakeFailed(UUID asset);
    }

    namespace
    {
        struct TextureBakeContext
        {
            UUID asset;
            u32  targetSize;
        };

        fs::path ThumbnailDiskPath(UUID asset)
        {
            return FileSystem::ProjectPath() / ".luth" / "thumbnails" / (asset.ToString() + ".png");
        }

        void BakeTexture(UUID asset, u32 targetSize)
        {
            // Snapshot the source path under no lock — AssetDatabase::GetMetadata
            // returns a const reference, but project switching could destroy the
            // entry under us. Copy the path immediately, validate after.
            fs::path srcPath;
            {
                const auto& meta = AssetDatabase::GetMetadata(asset);
                srcPath = meta.Path;
            }
            if (srcPath.empty() || !fs::exists(srcPath)) {
                LH_CORE_WARN("Thumbnail: source missing for {}", asset.ToString());
                ThumbnailCacheInternal::NotifyBakeFailed(asset);
                return;
            }

            int srcW = 0, srcH = 0, srcC = 0;
            stbi_uc* src = stbi_load(srcPath.string().c_str(), &srcW, &srcH, &srcC, 4);
            if (!src) {
                LH_CORE_WARN("Thumbnail: stbi_load failed for {}", srcPath.string());
                ThumbnailCacheInternal::NotifyBakeFailed(asset);
                return;
            }

            const u32 dstW = targetSize;
            const u32 dstH = targetSize;
            std::vector<u8> resized(static_cast<size_t>(dstW) * dstH * 4);
            const int rc = stbir_resize_uint8(
                src,             srcW, srcH, 0,
                resized.data(),  dstW, dstH, 0,
                4);
            stbi_image_free(src);
            if (rc == 0) {
                LH_CORE_WARN("Thumbnail: stbir_resize_uint8 failed for {}", asset.ToString());
                ThumbnailCacheInternal::NotifyBakeFailed(asset);
                return;
            }

            // Encode PNG into an in-memory buffer; persist via IOThread::WriteFile.
            // Disk persistence read-back lands in commit C — this commit just
            // produces the file as a side effect.
            std::vector<u8> pngBytes;
            const auto pngWriter = [](void* userCtx, void* data, int sz) {
                auto* out = static_cast<std::vector<u8>*>(userCtx);
                u8* p = static_cast<u8*>(data);
                out->insert(out->end(), p, p + static_cast<size_t>(sz));
            };
            const int wrote = stbi_write_png_to_func(
                pngWriter, &pngBytes,
                static_cast<int>(dstW), static_cast<int>(dstH), 4,
                resized.data(), static_cast<int>(dstW * 4));
            if (wrote && FileSystem::HasProject()) {
                const fs::path outPath = ThumbnailDiskPath(asset);
                std::error_code ec;
                fs::create_directories(outPath.parent_path(), ec);
                if (!ec)
                    IOThread::WriteFile(outPath.string(), std::vector<u8>(pngBytes));
            }

            ThumbnailCacheInternal::PushTextureCompletion(asset, std::move(resized), dstW, dstH);
        }

        void BakeTextureJobThunk(JobSystem::JobArgs args)
        {
            auto* ctx = static_cast<TextureBakeContext*>(args.data);
            BakeTexture(ctx->asset, ctx->targetSize);
            LH_DELETE(Memory::Category::Editor, ctx);
        }
    }

    void ThumbnailGenerator::Dispatch(UUID asset, AssetType type)
    {
        if (!asset.IsValid()) return;
        if (type != AssetType::Texture) return;   // mesh/material in commits E/F

        auto* ctx = LH_NEW(Memory::Category::Editor, TextureBakeContext);
        ctx->asset = asset;
        ctx->targetSize = 128;                    // settings-driven in commit G

        JobSystem::Execute(BakeTextureJobThunk, ctx, nullptr,
                           "Thumbnail.Bake.Tex", JobSystem::Priority::Low);
    }

    void ThumbnailGenerator::DispatchLoadFromDisk(UUID asset, AssetType /*type*/)
    {
        if (!asset.IsValid()) return;
        if (!FileSystem::HasProject()) return;

        const fs::path path = ThumbnailDiskPath(asset);
        // IOThread::ReadFile dispatches its callback onto a worker fiber after
        // the read completes — same execution context as a bake job, so the
        // decode + push-completion path mirrors BakeTexture exactly.
        IOThread::ReadFile(path.string(), [asset](std::vector<u8> bytes) {
            if (bytes.empty()) {
                ThumbnailCacheInternal::NotifyBakeFailed(asset);
                return;
            }
            int w = 0, h = 0, c = 0;
            stbi_uc* pixels = stbi_load_from_memory(
                bytes.data(), static_cast<int>(bytes.size()), &w, &h, &c, 4);
            if (!pixels) {
                LH_CORE_WARN("Thumbnail: stbi_load_from_memory failed for {}", asset.ToString());
                ThumbnailCacheInternal::NotifyBakeFailed(asset);
                return;
            }
            std::vector<u8> rgba(pixels, pixels + static_cast<size_t>(w) * h * 4);
            stbi_image_free(pixels);
            ThumbnailCacheInternal::PushTextureCompletion(asset, std::move(rgba),
                                                          static_cast<u32>(w),
                                                          static_cast<u32>(h));
        });
    }
}
