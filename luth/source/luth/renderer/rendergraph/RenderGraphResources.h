#pragma once

#include "luth/core/LuthTypes.h"
#include <string>
#include <variant>

namespace Luth::RG
{
    // ===================================================================================
    // Resource Handles
    // ===================================================================================
    
    struct ResourceHandle
    {
        u32 index = 0;
        u32 version = 0;

        bool IsValid() const { return index != 0; }
        bool operator==(const ResourceHandle& other) const { return index == other.index && version == other.version; }
        bool operator!=(const ResourceHandle& other) const { return !(*this == other); }
    };

    // ===================================================================================
    // Resource Descriptions
    // ===================================================================================

    enum class TextureFormat
    {
        Unknown = 0,
        RGBA8_Unorm,
        RGBA16_Float,
        RGBA32_Float,
        D32_Float,
        D24_Unorm_S8_Uint,
        // ... Add more as needed
    };

    struct TextureDesc
    {
        u32 width = 0;
        u32 height = 0;
        u32 depth = 1;
        u32 arrayLayers = 1;
        u32 mipLevels = 1;
        TextureFormat format = TextureFormat::Unknown;
        
        // If true, the size is relative to the swapchain (e.g., 1.0 = full screen, 0.5 = half res)
        bool isRelativeSize = false; 
        f32 sizeScale = 1.0f; 

        std::string name;
    };

    struct BufferDesc
    {
        u64 size = 0;
        std::string name;
    };

    // ===================================================================================
    // Synchronization & States
    // ===================================================================================

    enum class ResourceState
    {
        Undefined,
        General,
        ColorAttachment,
        DepthStencilAttachment,
        DepthStencilReadOnly,
        ShaderResource,
        UnorderedAccess, // Compute/Storage
        TransferSrc,
        TransferDst,
        Present
    };

    struct Barrier
    {
        ResourceHandle resource;
        ResourceState before;
        ResourceState after;
        // TODO: Add queue family ownership transfer info
    };

    // ===================================================================================
    // Resource Access Types (Metadata for Pass)
    // ===================================================================================

    enum class ResourceAccess
    {
        None,
        Read,       // Shader Resource (SRV)
        Write,      // Render Target (RTV) or UAV
        ReadWrite   // UAV
    };

}
