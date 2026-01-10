#include "luthpch.h"
#include "TextureImporter.h"
#include <stb/stb_image.h>

namespace Luth
{
    bool TextureImporter::Import(const std::filesystem::path& path, std::unique_ptr<AssetData>& outData)
    {
        LH_PROFILE_FUNCTION();

        int width, height, channels;
        stbi_set_flip_vertically_on_load(1);
        
        // Force 4 channels for now (RGBA)
        stbi_uc* data = stbi_load(path.string().c_str(), &width, &height, &channels, 4);
        
        if (!data)
        {
            LH_CORE_ERROR("TextureImporter: Failed to load image {0}", path.string());
            return false;
        }

        auto texData = std::make_unique<TextureAssetData>();
        texData->Width = width;
        texData->Height = height;
        texData->Format = TextureFormat::RGBA8;
        
        // Copy data to vector to own it
        size_t size = width * height * 4;
        texData->Pixels.resize(size);
        memcpy(texData->Pixels.data(), data, size);

        stbi_image_free(data);
        outData = std::move(texData);
        return true;
    }
}