#include "Luthpch.h"
#include "luth/renderer/openGL/GLTexture.h"
#include "luth/resources/FileSystem.h"
#include "luth/utils/ImageUtils.h"

#include <glad/glad.h>

namespace Luth
{
    GLTexture::GLTexture(const fs::path& path)
        : m_Path(FileSystem::GetPath(ResourceType::Texture, path))
    {
        LH_CORE_INFO("Creating GLTexture: {0}", m_Path.string());
        LoadFromFile();
    }

    GLTexture::GLTexture(u32 width, u32 height, u32 format, const unsigned char* data)
    {
        LH_CORE_INFO("Creating empty GLTexture ({0}x{1}, format {2})", width, height, static_cast<int>(format));
        CreateFromData(width, height, format, data);
    }

    GLTexture::~GLTexture()
    {
        LH_CORE_TRACE("Destroying GLTexture (ID: {0}, '{1}')", m_TextureID, m_Path.filename().string());
        glDeleteTextures(1, &m_TextureID);
    }

    void GLTexture::LoadFromFile()
    {
        int width, height, channels;
        stbi_set_flip_vertically_on_load(false);
        stbi_uc* data = stbi_load(m_Path.string().c_str(), &width, &height, &channels, 0);

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
            else if (channels == 1) {
                internalFormat = GL_R8;
                dataFormat = GL_RED;
            }

            if (internalFormat == 0) {
                LH_CORE_ERROR("Unsupported number of channels ({0}) in texture '{1}'", channels, m_Path.filename().string());
                stbi_image_free(data);
                return;
            }

            CreateInternal(internalFormat);

            glTextureSubImage2D(m_TextureID, 0, 0, 0, m_Width, m_Height, dataFormat, GL_UNSIGNED_BYTE, data);
            glGenerateTextureMipmap(m_TextureID);

            LH_CORE_TRACE("Created GLTexture '{0}' (ID: {1}, {2}x{3}, {4} channels, Mip levels: {5})",
                m_Path.filename().string(), m_TextureID, width, height, channels, m_MipLevels);
            stbi_image_free(data);
        }
        else {
            LH_CORE_ERROR("Failed to load texture from '{0}': {1}", m_Path.string(), stbi_failure_reason());
        }
    }

    void GLTexture::CreateInternal(GLenum internalFormat)
    {
        glCreateTextures(GL_TEXTURE_2D, 1, &m_TextureID);

        m_MipLevels = static_cast<int>(std::floor(std::log2(std::max(m_Width, m_Height)))) + 1;
        glTextureStorage2D(m_TextureID, m_MipLevels, internalFormat, m_Width, m_Height);

        glTextureParameteri(m_TextureID, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
        glTextureParameteri(m_TextureID, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTextureParameteri(m_TextureID, GL_TEXTURE_WRAP_S, GL_REPEAT);
        glTextureParameteri(m_TextureID, GL_TEXTURE_WRAP_T, GL_REPEAT);
    }

    void GLTexture::CreateFromData(u32 width, u32 height, u32 channels, const unsigned char* data)
    {
        // Determine OpenGL format
        GLenum internalFormat = GL_RGBA8;
        GLenum format = GL_RGBA;

        switch (channels) {
            case 1: internalFormat = GL_R8;    format = GL_RED;  break;
            case 3: internalFormat = GL_RGB8;  format = GL_RGB;  break;
            case 4: internalFormat = GL_RGBA8; format = GL_RGBA; break;
            default: LH_CORE_ASSERT(false, "Unsupported number of channels: {0}", channels);
        }

        // Create and configure texture
        glGenTextures(1, &m_TextureID);
        glBindTexture(GL_TEXTURE_2D, m_TextureID);

        // Set texture parameters
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);

        // Allocate storage and upload data
        glTexImage2D(GL_TEXTURE_2D, 0, internalFormat, width, height, 0, format, GL_UNSIGNED_BYTE, data);

        // Unbind texture
        glBindTexture(GL_TEXTURE_2D, 0);
    }

    void GLTexture::Bind(uint32_t slot) const
    {
        //LH_CORE_TRACE("Binding texture ID {0} to slot {1}", m_TextureID, slot);
        glActiveTexture(GL_TEXTURE0 + slot);
        glBindTexture(GL_TEXTURE_2D, m_TextureID);
    }
}
