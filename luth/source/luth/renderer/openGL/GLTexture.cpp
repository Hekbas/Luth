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

    GLTexture::GLTexture(u32 width, u32 height, TextureFormat format, const void* data)
    {
        LH_CORE_INFO("Creating empty GLTexture ({0}x{1}, format {2})", width, height, static_cast<int>(format));
        CreateFromData(width, height, format, data);
    }

    GLTexture::~GLTexture()
    {
        LH_CORE_TRACE("Destroying GLTexture (ID: {0}, '{1}')", m_TextureID, m_Path.filename().string());
        glDeleteTextures(1, &m_TextureID);
    }

    void GLTexture::Bind(uint32_t slot) const
    {
        //LH_CORE_TRACE("Binding texture ID {0} to slot {1}", m_TextureID, slot);
        glActiveTexture(GL_TEXTURE0 + slot);
        glBindTexture(GL_TEXTURE_2D, m_TextureID);
    }

    void GLTexture::SetWrapMode(TextureWrapMode mode)
    {
        m_WrapMode = mode;
        GLenum glWrap = GL_REPEAT;
        switch (mode) {
            case TextureWrapMode::Repeat:         glWrap = GL_REPEAT;          break;
            case TextureWrapMode::ClampToEdge:    glWrap = GL_CLAMP_TO_EDGE;   break;
            case TextureWrapMode::MirroredRepeat: glWrap = GL_MIRRORED_REPEAT; break;
        }

        glTextureParameteri(m_TextureID, GL_TEXTURE_WRAP_S, glWrap);
        glTextureParameteri(m_TextureID, GL_TEXTURE_WRAP_T, glWrap);
    }

    void GLTexture::SetFilterMode(TextureFilterMode min, TextureFilterMode mag)
    {
        m_MinFilter = min;
        m_MagFilter = mag;

        GLenum glMin = GL_LINEAR;
        switch (min) {
            case TextureFilterMode::Linear:               glMin = GL_LINEAR;                 break;
            case TextureFilterMode::Nearest:              glMin = GL_NEAREST;                break;
            case TextureFilterMode::LinearMipmapLinear:   glMin = GL_LINEAR_MIPMAP_LINEAR;   break;
            case TextureFilterMode::NearestMipmapNearest: glMin = GL_NEAREST_MIPMAP_NEAREST; break;
        }

        GLenum glMag = GL_LINEAR;
        switch (mag) {
            case TextureFilterMode::Linear:  glMag = GL_LINEAR;  break;
            case TextureFilterMode::Nearest: glMag = GL_NEAREST; break;
            default: break; // Mipmap filters not applicable for mag
        }

        glTextureParameteri(m_TextureID, GL_TEXTURE_MIN_FILTER, glMin);
        glTextureParameteri(m_TextureID, GL_TEXTURE_MAG_FILTER, glMag);
    }

    void GLTexture::GenerateMipmaps()
    {
        if (m_MipLevels > 1) {
            glGenerateTextureMipmap(m_TextureID);
        }
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
            switch (channels) {
			    case 1: internalFormat = GL_R8;      dataFormat = GL_RED;  break;
			    case 2: internalFormat = GL_RG8;     dataFormat = GL_RG;   break;
			    case 3: internalFormat = GL_RGB8;    dataFormat = GL_RGB;  break;
			    case 4: internalFormat = GL_RGBA8;   dataFormat = GL_RGBA; break;
                default: break;
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

    void GLTexture::CreateFromData(u32 width, u32 height, TextureFormat format, const void* data)
    {
        m_Width = width;
        m_Height = height;
        m_Format = format;

        GLenum internalFormat, dataFormat;
        GLenum dataType = GL_UNSIGNED_BYTE;

        switch (format) {
            case TextureFormat::R8:      internalFormat = GL_R8;      dataFormat = GL_RED;  break;
            case TextureFormat::RGB8:    internalFormat = GL_RGB8;    dataFormat = GL_RGB;  break;
            case TextureFormat::RGBA8:   internalFormat = GL_RGBA8;   dataFormat = GL_RGBA; break;
            case TextureFormat::RGBA32F: internalFormat = GL_RGBA32F; dataFormat = GL_RGBA; break;
            default: LH_CORE_ASSERT(false, "Unsupported texture format!"); return;
        }

        glCreateTextures(GL_TEXTURE_2D, 1, &m_TextureID);
        m_MipLevels = 1; // Default, can generate later

        glTextureStorage2D(m_TextureID, m_MipLevels, internalFormat, m_Width, m_Height);

        if (data) {
            glTextureSubImage2D(m_TextureID, 0, 0, 0, m_Width, m_Height, dataFormat, dataType, data);
        }

        // Apply default parameters
        SetWrapMode(m_WrapMode);
        SetFilterMode(m_MinFilter, m_MagFilter);
    }
}
