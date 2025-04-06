#pragma once

#include "luth/renderer/Texture.h"

typedef unsigned int GLenum;

namespace Luth
{
    class GLTexture : public Texture
    {
    public:
        GLTexture(const fs::path& path);
        GLTexture(u32 width, u32 height, u32 format, const unsigned char* data, const std::string& name);
        ~GLTexture();

        void Bind(uint32_t slot = 0) const override;
        //void SetData(void* data, uint32_t size) override;

        uint32_t GetWidth() const override { return m_Width; }
        uint32_t GetHeight() const override { return m_Height; }
        uint32_t GetRendererID() const override { return m_TextureID; }
        const fs::path& GetPath() const override { return m_Path; }

    private:
        void LoadFromFile();
        void CreateInternal(GLenum internalFormat);

        void CreateFromData(uint32_t width, uint32_t height,
            uint32_t channels, const unsigned char* data);

        uint32_t m_TextureID = 0;
        uint32_t m_Width = 0, m_Height = 0;
        int m_MipLevels = 0;
        TextureFormat m_Format = TextureFormat::RGBA8;
        fs::path m_Path;
    };
}
