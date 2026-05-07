#pragma once

#include "luth/resources/AssetImporter.h"
#include "luth/renderer/resources/Texture.h"
#include <vector>

namespace Luth
{
    // Converts source images (.png, .jpg, .tga, .hdr, ...) into Library/-resident
    // TextureAssetData artifacts that AssetManager loads at runtime. Decode and mipmap generation
    // run on worker fibers through JobSystem so import doesn't stall the editor frame.
    struct TextureAssetData : public AssetData
    {
        std::vector<unsigned char> Pixels;
        uint32_t Width = 0;
        uint32_t Height = 0;
        TextureFormat Format = TextureFormat::None;
        TextureSettings Settings;
    };

    class TextureImporter : public AssetImporter
    {
    public:
        bool Import(const std::filesystem::path& source, const std::filesystem::path& destination) override;
    };
}