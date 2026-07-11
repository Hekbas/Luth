#pragma once

#include "luth/core/types/LuthTypes.h"
#include "luth/resources/Asset.h"
#include <stb/stb_image.h>

#include <memory>

namespace Luth
{
    // Abstract GPU image asset. The concrete VKTexture implementation handles the
    // VkImage / VkImageView / VkSampler trio plus bindless-slot registration. Bind sites query
    // GetBindlessIndex() and shaders index into the global descriptor array; legacy Bind(slot)
    // remains as a no-op for shape compatibility with non-Vulkan call sites.
    enum class TextureFormat {
        None = 0,
        R8, RGB8, RGBA8, RGBA16F, RGBA32F,
        RG16F,
        R32_Float,                    // Compute storage (GTAO linear depth, etc.)
        D32_Float, D24_Unorm_S8_Uint, // Depth formats
        R32_Uint,
        R16_Uint,                     // Slim G-buffer material ID (16-bit, fits 16384-entry material SSBO)

        // Block-compressed imported material textures. Appended so existing serialized enum values stay
        // stable. UNORM only (appearance-preserving); a future sRGB pass maps color roles to _SRGB in
        // ToVkFormat with no re-encode.
        BC1_Unorm,  // RGB (opaque albedo), 8 B/block
        BC4_Unorm,  // single-channel R mask, 8 B/block
        BC5_Unorm,  // RG normals (Z reconstructed in-shader), 16 B/block
        BC7_Unorm   // RGBA high quality (default color/ORM), 16 B/block
    };

    // Block/layout facts for the compressed-upload + artifact paths. Uncompressed formats report
    // blockDim = 1 and blockBytes = bytes-per-texel so one level-size formula serves both.
    struct TextureFormatInfo {
        bool compressed;
        u32  blockDim;    // texels per block edge
        u32  blockBytes;  // bytes per 4x4 block (per texel when uncompressed)
    };

    TextureFormatInfo GetTextureFormatInfo(TextureFormat fmt);

    // Byte size of mip level (w,h). BCn: ceil(w/4)*ceil(h/4)*blockBytes; sub-4x4 levels use one block.
    u64 TextureLevelBytes(TextureFormat fmt, u32 w, u32 h);

    enum class TextureWrapMode {
        Repeat, ClampToEdge, MirroredRepeat
    };

    enum class TextureFilterMode {
        Linear, Nearest,
        LinearMipmapLinear, NearestMipmapNearest
    };

    // How a source texture's channels map onto the canonical material decode (material.slang fixed
    // swizzles: metalRough .g/.b, occlusion .r, normal *2-1). Drives an import-time pixel transform so
    // the artifact always carries canonical bytes. A future sRGB pass can branch sRGB-vs-UNORM off this
    // (Color is sRGB, data roles are UNORM). Default Color leaves pixels untouched.
    enum class TextureRole : u8 {
        Color = 0,         // albedo / emissive / color, no transform
        NormalGL,          // tangent-space normal +Y (OpenGL), no transform
        NormalDX,          // tangent-space normal -Y (DirectX): green flipped to +Y at import
        LinearData,        // single-channel linear (AO / roughness / metallic), no transform
        GlossToRoughness   // perceptual gloss in the roughness channel: inverted to roughness at import
    };

    struct TextureSettings
    {
        bool GenerateMipmaps = true;
        TextureWrapMode WrapMode = TextureWrapMode::Repeat;
        TextureFilterMode MinFilter = TextureFilterMode::Linear;
        TextureFilterMode MagFilter = TextureFilterMode::Linear;
        // Import-time only: the role's pixel transform bakes into the artifact, so it is not echoed in
        // TextureHeader (no runtime/GPU need). The editor reads it back from the .meta sidecar.
        TextureRole Role = TextureRole::Color;
    };

    class Texture : public Asset
    {
    public:
        virtual AssetType GetType() const override { return AssetType::Texture; }
        
        virtual ~Texture() = default;

        virtual void Bind(u32 slot = 0) const = 0;

        virtual u32 GetWidth() const = 0;
        virtual u32 GetHeight() const = 0;
        virtual u32 GetDepth() const { return 1; }
        virtual u32 GetRendererID() const = 0;
        virtual const fs::path& GetPath() const = 0;

        virtual TextureFormat GetFormat() const = 0;
        virtual std::string GetFormatString() const = 0;

        virtual TextureWrapMode GetWrapMode() const = 0;
        virtual void SetWrapMode(TextureWrapMode mode) = 0;

        virtual std::pair<TextureFilterMode, TextureFilterMode> GetFilterMode() const = 0;
        virtual void SetFilterMode(TextureFilterMode min, TextureFilterMode mag) = 0;

        virtual int GetMipLevels() const = 0;
        virtual void GenerateMipmaps() = 0;
        
        virtual u32 GetBindlessIndex() const { return 0; }

        // No path overload; the asset pipeline uses the data-taking form.
        static std::shared_ptr<Texture> Create(u32 width, u32 height,
            TextureFormat format, const void* data = nullptr);
        static std::shared_ptr<Texture> Create(u32 width, u32 height,
            TextureFormat format, const void* data, const TextureSettings& settings);
        // Pre-baked compressed form: data holds the full concatenated BCn mip chain (sizeBytes across
        // mipLevels levels). Only the asset pipeline produces this; render targets never compress.
        static std::shared_ptr<Texture> Create(u32 width, u32 height, TextureFormat format,
            const void* data, u64 sizeBytes, u32 mipLevels, const TextureSettings& settings);
    };
}
