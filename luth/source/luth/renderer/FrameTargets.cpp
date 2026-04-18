#include "luthpch.h"
#include "luth/renderer/FrameTargets.h"
#include "luth/renderer/resources/Texture.h"

namespace Luth
{
    void FrameTargets::Allocate(u32 width, u32 height)
    {
        m_SceneColor     = Texture::Create(width, height, TextureFormat::RGBA16F);
        m_SceneDepth     = Texture::Create(width, height, TextureFormat::D32_Float);
        m_LDROutput      = Texture::Create(width, height, TextureFormat::RGBA8);
        m_EntityIDBuffer = Texture::Create(width, height, TextureFormat::R32_Uint);
        m_SelectionMask  = Texture::Create(width, height, TextureFormat::RGBA8);
        m_SelectionDepth = Texture::Create(width, height, TextureFormat::D32_Float);
    }

    void FrameTargets::Resize(u32 width, u32 height)
    {
        // Guard against unsigned underflow from negative float→u32 casts at startup,
        // and mirror the existing clamp (skip when targets aren't initialised yet).
        if (!m_SceneColor || width == 0 || height == 0 || width > 16384 || height > 16384)
            return;
        Allocate(width, height);
    }
}
