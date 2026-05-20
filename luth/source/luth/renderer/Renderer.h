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
        
        // Single-graph convenience: Begin + Record + End in one call. Single view (slot 0, isLastView = true).
        static void ExecuteGraph(RG::RenderGraph& graph, u64 frameIndex, GPUTimerPool* timers = nullptr);

        // Multi-view path — each view owns its own QueueRecorders triplet (gA / compute / gB primary cmd buffers),
        // submitted in per-view 3-submit topology. Inter-view ordering is enforced by timeline-semaphore waits at
        // submit boundaries (replaces the legacy inline pipeline barrier between views). Pattern:
        //     for (u32 v = 0; v < N; ++v) {
        //         auto r = Renderer::BeginPrimaryCmd(frameIndex, v);
        //         bool hasCompute = Renderer::RecordGraph(r, rg, timers);
        //         Renderer::EndPrimaryCmdAndSubmit(r, frameIndex, v, hasCompute, /*isLastView=*/ v == N-1);
        //     }
        // RecordGraph returns true iff the graph routed any pass to the async-compute primary — forwarded to the
        // backend's SubmitView to decide whether to issue the compute submit at all.
        static QueueRecorders BeginPrimaryCmd(u64 frameIndex, u32 viewSlot);
        static bool RecordGraph(QueueRecorders recorders, RG::RenderGraph& graph, GPUTimerPool* timers = nullptr);
        static void EndPrimaryCmdAndSubmit(QueueRecorders recorders, u64 frameIndex, u32 viewSlot,
                                           bool hasComputeWork, bool isLastView);

        static void OnResize(u32 width, u32 height);

        static RenderBackend* GetBackend() { return s_Backend.get(); }
        static FrameData* GetFrameData() { return s_FrameData; }

    private:
        static std::unique_ptr<RenderBackend> s_Backend;
        static FrameData* s_FrameData; // Owned by App, NOT by Renderer
    };
}
