#pragma once

#include "luth/core/types/LuthTypes.h"
#include <string>
#include <vulkan/vulkan.h>

namespace Luth::RG
{
    // Render-graph handle types and the resource-state enum. ResourceHandle and BufferHandle
    // are versioned indices: versioning lets resource aliasing track distinct logical writes
    // to the same physical resource. ResourceState drives the barrier solver via the
    // GetStateInfo lookup in RenderGraph.cpp.
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
        ShaderResource,             // Fragment Shader Read
        ColorAttachment,            // Color Write
        DepthStencilAttachment,     // Depth Write
        TransferSrc,                // Copy Source
        TransferDst,                // Copy Dest / Clear
        Present,                    // Swapchain Present
        ComputeRead,                // Compute shader read (storage image)
        ComputeWrite,               // Compute shader write (storage image)
        StorageBufferRead,          // Compute shader read (SSBO)
        StorageBufferWrite,         // Compute shader write (SSBO)
        IndirectRead,               // Indirect draw/dispatch command read
        AccelerationStructureBuild, // vkCmdBuildAccelerationStructuresKHR write
        AccelerationStructureRead,  // Ray-query / RT-pipeline / fragment-shader read of AS
    };

    // Which queue family a pass executes on. Default Graphics — opt into AsyncCompute via the 4-arg
    // AddComputePass overload. SolveBarriers detects cross-queue handoffs and substitutes TOP_OF_PIPE for the
    // cross-family src stage on the reader's pre-barrier (cross-queue semaphore at submit covers the actual
    // memory dependency per Vulkan spec). See docs/development/arch/multi-queue.md.
    enum class QueueFamily : u8
    {
        Graphics     = 0,
        AsyncCompute = 1,
    };

    enum class TextureFormat
    {
        RGBA8_Unorm,
        BGRA8_Unorm, // Added for Swapchain
        R8_Unorm,     // Compute storage (GTAO raw/final AO, etc.)
        RGBA16_Float, // HDR render target
        RG16_Float,   // Slim G-buffer normal (octahedral) + motion vectors
        R32_Float,    // Compute storage (GTAO linear depth, etc.)
        D32_Float,
        D24_Unorm_S8_Uint,
        R32_Uint,
        R16_Uint,     // Slim G-buffer material ID
    };

    struct TextureDesc
    {
        std::string name;
        u32 width = 0;
        u32 height = 0;
        TextureFormat format = TextureFormat::RGBA8_Unorm;
        // 0 means "infer from format" (color-attachment / depth-stencil + sampled + transfer).
        // Pass an explicit set for compute-storage transients or other non-default uses.
        VkImageUsageFlags usage = 0;
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
        // True iff the previous writer ran on a different queue family than this barrier's pass. Drives the
        // cross-queue rule in RenderGraph::Execute: src stage / access are forced to TOP_OF_PIPE / NONE because
        // the cross-queue semaphore at submit time carries the actual memory dependency (per Vulkan spec).
        // Without this, a barrier emitted on the compute primary with a graphics-only src stage like
        // VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT would violate VUID-vkCmdPipelineBarrier2-srcStageMask-*.
        bool crossQueueSrc = false;
    };

    struct BufferBarrier
    {
        BufferHandle resource;
        ResourceState before;
        ResourceState after;
        bool crossQueueSrc = false;
    };
}