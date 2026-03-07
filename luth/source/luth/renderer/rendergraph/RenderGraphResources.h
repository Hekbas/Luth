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

    enum class ResourceState
    {
        Undefined,
        ShaderResource,         // Fragment Shader Read
        ColorAttachment,        // Color Write
        DepthStencilAttachment, // Depth Write
        TransferSrc,            // Copy Source
        TransferDst,            // Copy Dest / Clear
        Present                 // Swapchain Present
    };

    enum class TextureFormat
    {
        RGBA8_Unorm,
        BGRA8_Unorm, // Added for Swapchain
        D32_Float,
        D24_Unorm_S8_Uint,
        // Add more as needed
    };

    struct TextureDesc
    {
        std::string name;
        u32 width = 0;
        u32 height = 0;
        TextureFormat format = TextureFormat::RGBA8_Unorm;
    };

    struct Barrier
    {
        ResourceHandle resource;
        ResourceState before;
        ResourceState after;
    };
}