#pragma once

#include "luth/resources/AssetImporter.h"
#include "luth/renderer/Texture.h"
#include <vector>

namespace Luth
{
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