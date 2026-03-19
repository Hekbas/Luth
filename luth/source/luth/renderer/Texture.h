#pragma once

#include "luth/core/LuthTypes.h"
#include "luth/resources/Asset.h"
#include <stb/stb_image.h>

#include <memory>

namespace Luth
{
    enum class TextureFormat {
        None = 0,
        R8, RGB8, RGBA8, RGBA32F,
        D32_Float, D24_Unorm_S8_Uint // Added Depth formats
    };

    enum class TextureWrapMode {
        Repeat, ClampToEdge, MirroredRepeat
    };

    enum class TextureFilterMode {
        Linear, Nearest,
        LinearMipmapLinear, NearestMipmapNearest
    };

    struct TextureSettings
    {
        bool GenerateMipmaps = true;
        TextureWrapMode WrapMode = TextureWrapMode::Repeat;
        TextureFilterMode MinFilter = TextureFilterMode::Linear;
        TextureFilterMode MagFilter = TextureFilterMode::Linear;
    };

    class Texture : public Asset
    {
    public:
        virtual AssetType GetType() const override { return AssetType::Texture; }
        
        virtual ~Texture() = default;

        virtual void Bind(u32 slot = 0) const = 0;
        //virtual void SetData(void* data, u32 size) = 0;

        virtual u32 GetWidth() const = 0;
        virtual u32 GetHeight() const = 0;
        virtual u32 GetRendererID() const = 0;
        virtual const fs::path& GetPath() const = 0;

        virtual TextureFormat GetFormat() const = 0;
        virtual std::string GetFormatString() const = 0;

        virtual TextureWrapMode GetWrapMode() const = 0;
        virtual void SetWrapMode(TextureWrapMode mode) = 0;

        virtual std::pair<TextureFilterMode, TextureFilterMode> GetFilterMode() const = 0;
        virtual void SetFilterMode(TextureFilterMode min, TextureFilterMode mag) = 0;

        virtual int GetMipLevels() const = 0;
        virtual void GenerateMipmaps() = 0;
        
        // Bindless Support
        virtual u32 GetBindlessIndex() const { return 0; }

        static std::shared_ptr<Texture> Create(const fs::path& path);
        static std::shared_ptr<Texture> Create(u32 width, u32 height,
            TextureFormat format, const void* data = nullptr);
        static std::shared_ptr<Texture> Create(u32 width, u32 height,
            TextureFormat format, const void* data, const TextureSettings& settings);
    };
}
