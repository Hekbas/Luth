#pragma once

#include "luth/core/types/LuthTypes.h"
#include "luth/renderer/RenderBackend.h"
#include "luth/renderer/QueueRecorders.h"
#include "luth/core/FrameData.h"

#include <memory>

namespace Luth
{
    namespace RG { class RenderGraph; }
    class GPUTimerPool;

    // Static facade over the active RenderBackend. Holds the global FrameData pointer (owned by
    // App), forwards BeginFrame / EndFrame / GetCommandBuffer through to the backend, and exposes
    // the deletion queues that subsystems push retired GPU resources into. Renderer never owns
    // GPU lifetimes itself — those live on RenderPipeline + the various subsystems.
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

        // false = skip this frame (caller yields + retries).
        static bool BeginFrame(u64 frameIndex);
        static void EndFrame();
        
        // Single-graph convenience: Begin + Record + End in one call.
        static void ExecuteGraph(RG::RenderGraph& graph, u64 frameIndex, GPUTimerPool* timers = nullptr);

        // Multi-graph path. Record N graphs into the per-view QueueRecorders triplet; one submit per primary per
        // view + one present per frame. Pattern:
        //     auto recorders = Renderer::BeginPrimaryCmd(frameIndex);
        //     for (view : views) Renderer::RecordGraph(recorders, rg, timers);
        //     Renderer::EndPrimaryCmdAndSubmit(recorders, frameIndex);
        // RecordGraph returns true if the graph routed any pass to the async-compute primary — caller forwards
        // this to the backend's per-view submit logic.
        static QueueRecorders BeginPrimaryCmd(u64 frameIndex);
        static bool RecordGraph(QueueRecorders recorders, RG::RenderGraph& graph, GPUTimerPool* timers = nullptr);
        static void EndPrimaryCmdAndSubmit(QueueRecorders recorders, u64 frameIndex);

        static void OnResize(u32 width, u32 height);

        static RenderBackend* GetBackend() { return s_Backend.get(); }
        static FrameData* GetFrameData() { return s_FrameData; }

    private:
        static std::unique_ptr<RenderBackend> s_Backend;
        static FrameData* s_FrameData; // Owned by App, NOT by Renderer
    };
}
