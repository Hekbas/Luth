#pragma once

#include "luth/core/types/LuthTypes.h"
#include <memory>

namespace Luth
{
    class RenderBackend
    {
    public:
        enum class API
        {
            None = 0,
            Vulkan
        };

        virtual ~RenderBackend() = default;

        virtual void Init(void* windowHandle) = 0;
        virtual void Shutdown() = 0;

        // false = skip this frame (caller yields + retries with the same frameIndex).
        virtual bool AcquireImage(u64 frameIndex) = 0;

        // No-op if AcquireImage skipped.
        virtual void SubmitFrame(u64 frameIndex, void* commandBuffer) = 0;
        
        // Gets a dedicated Primary Command Buffer for the frame submission
        virtual void* GetFrameCommandBuffer(u64 frameIndex) = 0;

        virtual void OnResize(u32 width, u32 height) = 0;

        static API GetAPI() { return s_API; }
        static std::unique_ptr<RenderBackend> Create();

    protected:
        static API s_API;
    };
}
