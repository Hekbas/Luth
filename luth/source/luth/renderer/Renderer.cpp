#include "luthpch.h"
#include "luth/renderer/Renderer.h"
#include "luth/renderer/OpenGL/GLRendererAPI.h"
#include "luth/renderer/vulkan/VKRendererAPI.h"

namespace Luth
{
    std::unique_ptr<RendererAPI> Renderer::s_RendererAPI = nullptr;

    void Renderer::Init(RendererAPI::API api, void* window)
    {
        s_RendererAPI = RendererAPI::Create(api);
        s_RendererAPI->SetWindow(window);
        s_RendererAPI->Init();
    }

    void Renderer::Shutdown()
    {
        if (s_RendererAPI) {
            s_RendererAPI->Shutdown();
            s_RendererAPI.reset();
        }
    }

    // Forwarding commands
    void Renderer::SetViewport(uint32_t x, uint32_t y, uint32_t w, uint32_t h) {
        s_RendererAPI->SetViewport(x, y, w, h);
    }

    void Renderer::SetClearColor(const glm::vec4& color) {
        s_RendererAPI->SetClearColor(color);
    }

    void Renderer::Clear() {
        s_RendererAPI->Clear();
    }

    void Renderer::SubmitMesh(const std::shared_ptr<Mesh>& mesh) {
        s_RendererAPI->SubmitMesh(mesh);
    }

    void Renderer::DrawIndexed(uint32_t count) {
        s_RendererAPI->DrawIndexed(count);
    }

    void Renderer::DrawFrame() {
        s_RendererAPI->DrawFrame();
    }
}
