#include "luthpch.h"
#include "luth/renderer/Renderer.h"
#include "luth/renderer/MaterialSystem.h"

namespace Luth
{
    std::unique_ptr<RendererAPI> Renderer::s_RendererAPI = nullptr;

    void Renderer::Init(void* windowHandle)
    {
        s_RendererAPI = RendererAPI::Create();
        s_RendererAPI->Init(windowHandle);
        
        // Init Material System (Global SSBO)
        MaterialSystem::Init();
    }

    void Renderer::Shutdown()
    {
        MaterialSystem::Shutdown();

        if (s_RendererAPI) {
            s_RendererAPI->Shutdown();
            s_RendererAPI.reset();
        }
    }

    bool Renderer::BeginFrame()
    {
        return s_RendererAPI->BeginFrame();
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
