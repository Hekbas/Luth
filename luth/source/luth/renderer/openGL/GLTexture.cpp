#include "Luthpch.h"
#include "luth/renderer/openGL/GLTexture.h"
#include "luth/utils/ImageUtils.h"

#include <glad/glad.h>

namespace Luth
{
    GLTexture::GLTexture(const fs::path& path)
        : m_Path(ResourceManager::GetPath(Resource::Texture, path))
    {
        LoadFromFile();
    }

    GLTexture::GLTexture(uint32_t width, uint32_t height, TextureFormat format) {}

    GLTexture::~GLTexture() {}

    void GLTexture::LoadFromFile()
    {
        int width, height, channels;
        stbi_set_flip_vertically_on_load(false);
        stbi_uc* data = stbi_load(m_Path.string().c_str(),
            &width, &height, &channels, 0);

        if (data) {
            m_Width = width;
            m_Height = height;

            GLenum internalFormat = 0, dataFormat = 0;
            if (channels == 4) {
                internalFormat = GL_RGBA8;
                dataFormat = GL_RGBA;
            }
            else if (channels == 3) {
                internalFormat = GL_RGB8;
                dataFormat = GL_RGB;
            }

            CreateInternal(internalFormat);

            glTextureSubImage2D(m_RendererID, 0, 0, 0,
                m_Width, m_Height, dataFormat, GL_UNSIGNED_BYTE, data);
            glGenerateTextureMipmap(m_RendererID);

            stbi_image_free(data);
        }
    }

    void GLTexture::CreateInternal(GLenum internalFormat)
    {
        glCreateTextures(GL_TEXTURE_2D, 1, &m_RendererID);

        // Calculate mip levels
        m_MipLevels = static_cast<int>(std::floor(std::log2(std::max(m_Width, m_Height)))) + 1;

        // Allocate storage with mip levels (increased 33%)
        glTextureStorage2D(m_RendererID,
            m_MipLevels,
            internalFormat,
            m_Width,
            m_Height
        );

        // Set mipmapped filtering
        glTextureParameteri(m_RendererID, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
        glTextureParameteri(m_RendererID, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTextureParameteri(m_RendererID, GL_TEXTURE_WRAP_S, GL_REPEAT);
        glTextureParameteri(m_RendererID, GL_TEXTURE_WRAP_T, GL_REPEAT);
    }

    void GLTexture::Bind(uint32_t slot) const
    {
        glActiveTexture(GL_TEXTURE0 + slot);
        glBindTexture(GL_TEXTURE_2D, m_RendererID);
    }
}
