#pragma once

#include "luth/core/LuthTypes.h"
#include "luth/renderer/RenderBackend.h"
#include "luth/core/FrameData.h"

#include <memory>

namespace Luth
{
    namespace RG { class RenderGraph; }

    class Renderer
    {
    public:
        static void Init(void* windowHandle);
        static void Shutdown();

        // New Frame Logic
        static void BeginFrame();
        static void EndFrame();
        
        static void ExecuteGraph(RG::RenderGraph& graph);
        static void OnResize(u32 width, u32 height);

        static RenderBackend* GetBackend() { return s_Backend.get(); }
        static FrameContext& GetCurrentFrame() { return s_FrameData.GetCurrentFrame(); }

    private:
        static std::unique_ptr<RenderBackend> s_Backend;
        static FrameData s_FrameData;
    };
}
