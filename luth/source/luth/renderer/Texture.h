#pragma once

#include "luth/core/LuthTypes.h"
#include "luth/resources/Resource.h"
#include "luth/utils/ImageUtils.h"

#include <memory>

namespace Luth
{
    enum class TextureFormat {
        None = 0,
        RGB8, RGBA8, RGBA32F,
    };

    enum class TextureWrapMode {
        Repeat, ClampToEdge, MirroredRepeat
    };

    enum class TextureFilterMode {
        Linear, Nearest,
        LinearMipmapLinear, NearestMipmapNearest
    };

    class Texture : public Resource
    {
    public:
        virtual ~Texture() = default;

        virtual void Bind(u32 slot = 0) const = 0;
        //virtual void SetData(void* data, u32 size) = 0;

        virtual u32 GetWidth() const = 0;
        virtual u32 GetHeight() const = 0;
        virtual u32 GetRendererID() const = 0;
        virtual const fs::path& GetPath() const = 0;

        static std::shared_ptr<Texture> Create(const fs::path& path);
        static std::shared_ptr<Texture> Create(u32 width, u32 height, TextureFormat format = TextureFormat::RGBA8);
    };
}
