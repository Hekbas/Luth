#pragma once

#include "luth/core/LuthTypes.h"
#include "luth/renderer/RendererAPI.h"

#include <memory>

namespace Luth
{
    namespace RG { class RenderGraph; }

    class Renderer
    {
    public:
        static void Init(void* windowHandle);
        static void Shutdown();

        static void BeginFrame();
        static void EndFrame();
        
        static void ExecuteGraph(RG::RenderGraph& graph);
        static void OnResize(u32 width, u32 height);

        static RendererAPI::API GetAPI() { return RendererAPI::GetAPI(); }
        static RendererAPI* GetRendererAPI() { return s_RendererAPI.get(); }

    private:
        static std::unique_ptr<RendererAPI> s_RendererAPI;
    };
}
