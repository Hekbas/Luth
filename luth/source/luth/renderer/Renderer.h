#pragma once

#include "luth/core/types/LuthTypes.h"
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
        
        // Single-graph convenience: Begin + Record + End in one call.
        static void ExecuteGraph(RG::RenderGraph& graph, u64 frameIndex, GPUTimerPool* timers = nullptr);

        // Multi-graph path. Record N graphs into one primary command buffer;
        // one submit + present per frame. Pattern:
        //     auto cmd = Renderer::BeginPrimaryCmd(frameIndex);
        //     for (view : views) Renderer::RecordGraph(cmd, rg, timers);
        //     Renderer::EndPrimaryCmdAndSubmit(cmd, frameIndex);
        static void* BeginPrimaryCmd(u64 frameIndex);
        static void  RecordGraph(void* cmd, RG::RenderGraph& graph, GPUTimerPool* timers = nullptr);
        static void  EndPrimaryCmdAndSubmit(void* cmd, u64 frameIndex);

        static void OnResize(u32 width, u32 height);

        static RenderBackend* GetBackend() { return s_Backend.get(); }
        static FrameData* GetFrameData() { return s_FrameData; }

    private:
        static std::unique_ptr<RenderBackend> s_Backend;
        static FrameData* s_FrameData; // Owned by App, NOT by Renderer
    };
}
