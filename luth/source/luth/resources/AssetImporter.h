#pragma once

#include "luth/resources/Asset.h"
#include <filesystem>
#include <memory>

namespace Luth
{
    // Intermediate container for raw data loaded from disk (CPU side only)
    struct AssetData
    {
        virtual ~AssetData() = default;
    };

    class AssetImporter
    {
    public:
        virtual ~AssetImporter() = default;

        // Runs on Worker Thread. Must NOT touch Vulkan context.
        // Returns true if successful and populates outData.
        virtual bool Import(const std::filesystem::path& path, std::unique_ptr<AssetData>& outData) = 0;
    };
}