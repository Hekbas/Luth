#pragma once

#include "luth/resources/Asset.h"
#include <filesystem>
#include <memory>

namespace Luth
{
    // Base type for the in-memory asset payload an importer produces. CPU-side only; concrete
    // subclasses (TextureAssetData, ModelAssetData, ...) hold the decoded data the loader path
    // hands to the GPU upload step. AssetData survives just long enough to upload through
    // UploadContext; the loaded Asset holds GPU handles after that.
    struct AssetData
    {
        virtual ~AssetData() = default;
    };

    // Per-asset-type importer interface. Each concrete importer turns a source file into a
    // binary artifact under <project>/Library/. Import runs on a worker fiber and must not touch
    // the Vulkan context; GPU uploads happen at load time, not at import time.
    class AssetImporter
    {
    public:
        virtual ~AssetImporter() = default;

        // Runs on Worker Thread. Reads source, processes, and writes binary artifact to destination.
        // Must NOT touch Vulkan context.
        virtual bool Import(const std::filesystem::path& source, const std::filesystem::path& destination) = 0;
    };
}