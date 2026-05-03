#include "lepch.h"
#include "luthien/widgets/ThumbnailGenerator.h"
#include "luthien/widgets/ThumbnailCache.h"

#include "luth/core/diagnostics/Log.h"
#include "luth/jobs/IOThread.h"
#include "luth/jobs/JobSystem.h"
#include "luth/memory/MemoryMacros.h"
#include "luth/resources/AssetDatabase.h"
#include "luth/resources/FileSystem.h"
#include "luth/resources/Image.h"

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

            Image::LoadResult8 src = Image::Load(srcPath);
            if (!src.valid) {
                LH_CORE_WARN("Thumbnail: Image::Load failed for {}", srcPath.string());
                ThumbnailCacheInternal::NotifyBakeFailed(asset);
                return;
            }

            // Aspect-preserving downscale: largest source dim = targetSize, the
            // other scaled proportionally. Sources already smaller than targetSize
            // pass through unchanged (no upscaling for thumbnails).
            const u32 srcMax = std::max(src.width, src.height);
            u32 dstW, dstH;
            if (srcMax <= targetSize) {
                dstW = src.width;
                dstH = src.height;
            } else {
                dstW = std::max<u32>(1, static_cast<u32>(static_cast<u64>(src.width)  * targetSize / srcMax));
                dstH = std::max<u32>(1, static_cast<u32>(static_cast<u64>(src.height) * targetSize / srcMax));
            }
            std::vector<u8> resized(static_cast<size_t>(dstW) * dstH * 4);
            if ((dstW == src.width && dstH == src.height)) {
                resized = std::move(src.pixels);
            } else if (!Image::Resize(src.pixels.data(), src.width, src.height,
                                      resized.data(),     dstW,      dstH, 4)) {
                LH_CORE_WARN("Thumbnail: Image::Resize failed for {}", asset.ToString());
                ThumbnailCacheInternal::NotifyBakeFailed(asset);
                return;
            }

            // Encode + persist. The bake's PNG side effect is what ScanDiskCache
            // re-hydrates from on next project load.
            std::vector<u8> pngBytes = Image::EncodePngToMemory(resized.data(), dstW, dstH, 4);
            if (!pngBytes.empty() && FileSystem::HasProject()) {
                const fs::path outPath = ThumbnailDiskPath(asset);
                std::error_code ec;
                fs::create_directories(outPath.parent_path(), ec);
                if (!ec)
                    IOThread::WriteFile(outPath.string(), std::move(pngBytes));
            }

            ThumbnailCacheInternal::PushTextureCompletion(asset, std::move(resized), dstW, dstH);
        }

        void BakeTextureJobThunk(JobSystem::JobArgs args)
        {
            auto* ctx = static_cast<TextureBakeContext*>(args.data);
            const UUID asset = ctx->asset;
            try {
                BakeTexture(ctx->asset, ctx->targetSize);
            } catch (const std::exception& e) {
                LH_CORE_ERROR("Thumbnail: bake threw for {}: {}", asset.ToString(), e.what());
                ThumbnailCacheInternal::NotifyBakeFailed(asset);
            } catch (...) {
                LH_CORE_ERROR("Thumbnail: bake threw non-std exception for {}", asset.ToString());
                ThumbnailCacheInternal::NotifyBakeFailed(asset);
            }
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
            try {
                if (bytes.empty()) {
                    ThumbnailCacheInternal::NotifyBakeFailed(asset);
                    return;
                }
                Image::LoadResult8 img = Image::LoadFromMemory(bytes.data(), bytes.size());
                if (!img.valid) {
                    LH_CORE_WARN("Thumbnail: disk decode failed for {}", asset.ToString());
                    ThumbnailCacheInternal::NotifyBakeFailed(asset);
                    return;
                }
                ThumbnailCacheInternal::PushTextureCompletion(asset, std::move(img.pixels),
                                                              img.width, img.height);
            } catch (const std::exception& e) {
                LH_CORE_ERROR("Thumbnail: disk-load threw for {}: {}", asset.ToString(), e.what());
                ThumbnailCacheInternal::NotifyBakeFailed(asset);
            } catch (...) {
                LH_CORE_ERROR("Thumbnail: disk-load threw non-std exception for {}", asset.ToString());
                ThumbnailCacheInternal::NotifyBakeFailed(asset);
            }
        });
    }
}
