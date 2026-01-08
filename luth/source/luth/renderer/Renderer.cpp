#include "luthpch.h"
#include "luth/renderer/Renderer.h"

namespace Luth
{
    std::unique_ptr<RendererAPI> Renderer::s_RendererAPI = nullptr;

    void Renderer::Init()
    {
        s_RendererAPI = RendererAPI::Create();
        s_RendererAPI->Init();
    }

    void Renderer::Shutdown()
    {
        if (s_RendererAPI) {
            s_RendererAPI->Shutdown();
            s_RendererAPI.reset();
        }
    }

    void Renderer::BeginFrame()
    {
        s_RendererAPI->BeginFrame();
    }

    void Renderer::EndFrame()
    {
        s_RendererAPI->EndFrame();
    }

    void Renderer::OnResize(u32 width, u32 height)
    {
        s_RendererAPI->OnResize(width, height);
    }

    void Renderer::ExecuteGraph(RG::RenderGraph& graph)
    {
        s_RendererAPI->ExecuteGraph(graph);
    }
}
