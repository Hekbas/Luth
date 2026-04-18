#pragma once

#include "luth/core/LuthTypes.h"
#include "luth/renderer/RenderBackend.h"
#include "luth/core/FrameData.h"

#include <memory>

namespace Luth
{
    namespace RG { class RenderGraph; }
    class GPUTimerPool;

    class Renderer
    {
    public:
        static void Init(void* windowHandle);
        static void Shutdown();

        // Call before destroying any GPU resources outside the render loop
        static void WaitForGPU();
        static void FlushDeletionQueues();

        // Frame management — Renderer uses FrameData owned by App
        static void SetFrameData(FrameData* frameData);

        // New Frame Logic
        static void BeginFrame(u64 frameIndex);
        static void EndFrame();
        
        static void ExecuteGraph(RG::RenderGraph& graph, u64 frameIndex, GPUTimerPool* timers = nullptr);
        static void OnResize(u32 width, u32 height);

        static RenderBackend* GetBackend() { return s_Backend.get(); }
        static FrameData* GetFrameData() { return s_FrameData; }

    private:
        static std::unique_ptr<RenderBackend> s_Backend;
        static FrameData* s_FrameData; // Owned by App, NOT by Renderer
    };
}
