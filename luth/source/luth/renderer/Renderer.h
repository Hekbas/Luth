#pragma once

#include "luth/core/LuthTypes.h"
#include "luth/renderer/RendererAPI.h"

#include <glm/glm.hpp>
#include <memory>


namespace Luth
{
    class Renderer
    {
    public:
        static void Init(RendererAPI::API api, void* window);
        static void Shutdown();

        static void SetViewport(uint32_t x, uint32_t y, uint32_t width, uint32_t height);
        static void SetClearColor(const glm::vec4& color);
        static void Clear();
        static void DrawIndexed(uint32_t count);
        static void DrawFrame();

        static RendererAPI::API GetAPI() { return RendererAPI::GetAPI(); }
        static RendererAPI* GetRendererAPI() { return s_RendererAPI.get(); }

    private:
        static std::unique_ptr<RendererAPI> s_RendererAPI;
    };
}
