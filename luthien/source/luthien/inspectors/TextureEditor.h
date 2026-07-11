#pragma once

#include "luth/core/UUID.h"

namespace Luth
{
    class Texture;

    // Inspector pane for Texture assets. Shows a preview plus sampler settings (wrap, filter,
    // mip generation); changes flush back through the import pipeline on Apply, which produces
    // a fresh artifact and reloads it through AssetManager.
    class TextureEditor
    {
    public:
        void Draw(Texture& texture);

    private:
        UUID m_LastTextureUUID;
        int  m_WrapMode = 0;
        int  m_MinFilter = 0;
        int  m_MagFilter = 0;
        bool m_GenerateMipmaps = true;
        int  m_Role = 0;               // TextureRole: channel layout / import transform
        int  m_Compression = 0;        // 0 Auto, then None/BC1/BC4/BC5/BC7 (kCompKeys)
        int  m_CompressionQuality = 1; // 0 Fast, 1 Normal, 2 High
    };
}
