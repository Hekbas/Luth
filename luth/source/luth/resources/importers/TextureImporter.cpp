#include "luthpch.h"
#include "TextureImporter.h"
#include "luth/resources/AssetSerializer.h"
#include <stb/stb_image.h>

namespace Luth
{
    bool TextureImporter::Import(const std::filesystem::path& source, const std::filesystem::path& destination)
    {
        LH_PROFILE_FUNCTION();

        int width, height, channels;
        stbi_set_flip_vertically_on_load(1);
        
        // Force 4 channels for now (RGBA)
        stbi_uc* data = stbi_load(source.string().c_str(), &width, &height, &channels, 4);
        
        if (!data)
        {
            LH_CORE_ERROR("TextureImporter: Failed to load image {0}", source.string());
            return false;
        }

        TextureAssetData texData;
        texData.Width = width;
        texData.Height = height;
        texData.Format = TextureFormat::RGBA8;
        
        // Copy data to vector to own it
        size_t size = width * height * 4;
        texData.Pixels.resize(size);
        memcpy(texData.Pixels.data(), data, size);

        stbi_image_free(data);
        
        return AssetSerializer::SerializeTexture(destination, texData);
    }
}