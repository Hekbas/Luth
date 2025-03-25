#include "Luthpch.h"
#include "luth/renderer/openGL/GLTexture.h"
#include "luth/utils/ImageUtils.h"

#include <glad/glad.h>

namespace Luth
{
    GLTexture::GLTexture(const fs::path& path)
        : m_Path(ResourceManager::GetPath(Resource::Texture, path))
    {
        LH_CORE_INFO("Creating texture from path: {0}", m_Path.string());
        LoadFromFile();
    }

    GLTexture::GLTexture(uint32_t width, uint32_t height, TextureFormat format)
    {
        LH_CORE_INFO("Creating empty texture ({0}x{1}, format {2})", width, height, static_cast<int>(format));
        // TODO: Implementation for empty texture creation should be added here
    }

    GLTexture::~GLTexture()
    {
        LH_CORE_TRACE("Destroying GLTexture (ID: {0}, '{1}')", m_TextureID, m_Path.filename().string());
        // TODO: Add glDeleteTextures if necessary
    }

    void GLTexture::LoadFromFile()
    {
        int width, height, channels;
        stbi_set_flip_vertically_on_load(false);
        stbi_uc* data = stbi_load(m_Path.string().c_str(), &width, &height, &channels, 0);

        if (data)
        {
            m_Width = width;
            m_Height = height;

            GLenum internalFormat = 0, dataFormat = 0;
            if (channels == 4)
            {
                internalFormat = GL_RGBA8;
                dataFormat = GL_RGBA;
            }
            else if (channels == 3)
            {
                internalFormat = GL_RGB8;
                dataFormat = GL_RGB;
            }

            if (internalFormat == 0)
            {
                LH_CORE_ERROR("Unsupported number of channels ({0}) in texture '{1}'", channels, m_Path.filename().string());
                stbi_image_free(data);
                return;
            }

            CreateInternal(internalFormat);

            glTextureSubImage2D(m_TextureID, 0, 0, 0, m_Width, m_Height, dataFormat, GL_UNSIGNED_BYTE, data);
            glGenerateTextureMipmap(m_TextureID);

            LH_CORE_INFO("Successfully loaded texture '{0}' ({1}x{2}, {3} channels)", m_Path.filename().string(), width, height, channels);
            stbi_image_free(data);
        }
        else
        {
            LH_CORE_ERROR("Failed to load texture from '{0}': {1}", m_Path.string(), stbi_failure_reason());
        }
    }

    void GLTexture::CreateInternal(GLenum internalFormat)
    {
        glCreateTextures(GL_TEXTURE_2D, 1, &m_TextureID);
        LH_CORE_TRACE("Created OpenGL texture with ID: {0}", m_TextureID);

        m_MipLevels = static_cast<int>(std::floor(std::log2(std::max(m_Width, m_Height)))) + 1;

        glTextureStorage2D(m_TextureID, m_MipLevels, internalFormat, m_Width, m_Height);
        LH_CORE_TRACE("Allocated texture storage for ID {0}: {1}x{2}, format {3}, mip levels {4}",
            m_TextureID, m_Width, m_Height, internalFormat, m_MipLevels);

        glTextureParameteri(m_TextureID, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
        glTextureParameteri(m_TextureID, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTextureParameteri(m_TextureID, GL_TEXTURE_WRAP_S, GL_REPEAT);
        glTextureParameteri(m_TextureID, GL_TEXTURE_WRAP_T, GL_REPEAT);
        LH_CORE_TRACE("Set texture parameters for ID {0}: min/mag filter, wrap S/T", m_TextureID);
    }

    void GLTexture::Bind(uint32_t slot) const
    {
        //LH_CORE_TRACE("Binding texture ID {0} to slot {1}", m_TextureID, slot);
        glActiveTexture(GL_TEXTURE0 + slot);
        glBindTexture(GL_TEXTURE_2D, m_TextureID);
    }
}
