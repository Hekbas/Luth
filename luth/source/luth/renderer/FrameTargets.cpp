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
        // Slim G-buffer: written by SlimGBufferPass; feeds TAA + downstream RT denoise.
        m_SlimNormal     = Texture::Create(width, height, TextureFormat::RG16F);
        m_SlimRoughness  = Texture::Create(width, height, TextureFormat::R8);
        m_SlimMotion     = Texture::Create(width, height, TextureFormat::RG16F);
        m_SlimMaterialID = Texture::Create(width, height, TextureFormat::R16_Uint);
    }

    void FrameTargets::Resize(u32 width, u32 height)
    {
        // Guard against unsigned underflow from negative float->u32 casts at startup,
        // and mirror the existing clamp (skip when targets aren't initialised yet).
        if (!m_SceneColor || width == 0 || height == 0 || width > 16384 || height > 16384)
            return;
        // No-op when size is unchanged: Allocate replaces every texture's shared_ptr
        // unconditionally, which would silently invalidate the size-keyed ViewResources cache.
        if (m_SceneColor->GetWidth() == width && m_SceneColor->GetHeight() == height)
            return;
        Allocate(width, height);
    }
}
