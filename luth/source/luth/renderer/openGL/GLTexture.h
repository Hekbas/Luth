#pragma once

#include "luth/renderer/Texture.h"

typedef unsigned int GLenum;

namespace Luth
{
    class GLTexture : public Texture
    {
    public:
        GLTexture(const fs::path& path);
        GLTexture(u32 width, u32 height, TextureFormat format, const void* data);
        ~GLTexture();

        void Bind(uint32_t slot = 0) const override;
        //void SetData(void* data, uint32_t size) override;

        uint32_t GetWidth() const override { return m_Width; }
        uint32_t GetHeight() const override { return m_Height; }
        uint32_t GetRendererID() const override { return m_TextureID; }
        const fs::path& GetPath() const override { return m_Path; }

        void SetWrapMode(TextureWrapMode mode) override;
        void SetFilterMode(TextureFilterMode min, TextureFilterMode mag) override;
        void GenerateMipmaps() override;

    private:
        void LoadFromFile();
        void CreateInternal(GLenum internalFormat);

        void CreateFromData(u32 width, u32 height, TextureFormat format, const void* data);

        uint32_t m_TextureID = 0;
        uint32_t m_Width = 0, m_Height = 0;
        int m_MipLevels = 0;
        TextureFormat m_Format = TextureFormat::RGBA8;
        TextureWrapMode m_WrapMode = TextureWrapMode::Repeat;
        TextureFilterMode m_MinFilter = TextureFilterMode::Linear;
        TextureFilterMode m_MagFilter = TextureFilterMode::Linear;
        fs::path m_Path;
    };
}
