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
        
        // Single-graph convenience — begins + records + submits one graph in
        // a self-contained call. Used when only one view renders (ExecuteMinimal
        // for the frame debugger's Frozen state, or legacy callers).
        static void ExecuteGraph(RG::RenderGraph& graph, u64 frameIndex, GPUTimerPool* timers = nullptr);

        // Multi-graph path — split so RS::Update can record multiple views
        // into one primary command buffer (single vkQueueSubmit + present
        // per frame). Pattern:
        //
        //     auto cmd = Renderer::BeginPrimaryCmd(frameIndex);
        //     for (view : visibleViews) { record the view's graph }
        //     Renderer::RecordGraph(cmd, graph, timers);
        //     Renderer::EndPrimaryCmdAndSubmit(cmd, frameIndex);
        //
        // EndPrimaryCmdAndSubmit attaches the present barrier + end + submit
        // + present. Every BeginPrimaryCmd call must be paired with an
        // EndPrimaryCmdAndSubmit in the same frame.
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
