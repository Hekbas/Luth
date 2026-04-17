#pragma once

#include "luth/core/LuthTypes.h"
#include <string>
#include <vulkan/vulkan.h>

namespace Luth::RG
{
    struct ResourceHandle
    {
        u32 index = 0;
        u32 version = 0;

        bool IsValid() const { return index != 0; }
        bool operator==(const ResourceHandle& other) const = default;
    };

    struct BufferHandle
    {
        u32 index = 0;
        u32 version = 0;

        bool IsValid() const { return index != 0; }
        bool operator==(const BufferHandle& other) const = default;
    };

    enum class ResourceState
    {
        Undefined,
        ShaderResource,         // Fragment Shader Read
        ColorAttachment,        // Color Write
        DepthStencilAttachment, // Depth Write
        TransferSrc,            // Copy Source
        TransferDst,            // Copy Dest / Clear
        Present,                // Swapchain Present
        ComputeRead,            // Compute shader read (storage image)
        ComputeWrite,           // Compute shader write (storage image)
        StorageBufferRead,      // Compute shader read (SSBO)
        StorageBufferWrite,     // Compute shader write (SSBO)
        IndirectRead,           // Indirect draw/dispatch command read
    };

    enum class TextureFormat
    {
        RGBA8_Unorm,
        BGRA8_Unorm, // Added for Swapchain
        R8_Unorm,     // Compute storage (GTAO raw/final AO, etc.)
        RGBA16_Float, // HDR render target
        R32_Float,    // Compute storage (GTAO linear depth, etc.)
        D32_Float,
        D24_Unorm_S8_Uint,
        R32_Uint,
    };

    struct TextureDesc
    {
        std::string name;
        u32 width = 0;
        u32 height = 0;
        TextureFormat format = TextureFormat::RGBA8_Unorm;
    };

    struct BufferDesc
    {
        std::string name;
        u64 size = 0;
        VkBufferUsageFlags usage = 0;
    };

    struct Barrier
    {
        ResourceHandle resource;
        ResourceState before;
        ResourceState after;
    };

    struct BufferBarrier
    {
        BufferHandle resource;
        ResourceState before;
        ResourceState after;
    };
}