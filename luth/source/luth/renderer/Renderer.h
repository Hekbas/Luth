#pragma once

#include "luth/core/LuthTypes.h"
#include "luth/core/Math.h"
#include "luth/renderer/RendererAPI.h"
#include "luth/renderer/Mesh.h"

#include <memory>


namespace Luth
{
    class Renderer
    {
    public:
        static void Init(RendererAPI::API api, void* window);
        static void Shutdown();

        static void BindFramebuffer(const std::shared_ptr<Framebuffer>& framebuffer);

        static void SetViewport(uint32_t x, uint32_t y, uint32_t width, uint32_t height);
        static void SetClearColor(const glm::vec4& color);
        static void Clear();

        static void EnableDepthMask(bool enable);
        static bool IsDepthMaskEnabled();

        static void EnableDepthTest(bool enable);
        static bool IsDepthTestEnabled() { return true; }
         
        static void EnableBlending(bool enable);
        static void SetBlendFunction(RendererAPI::BlendFactor srcFactor, RendererAPI::BlendFactor dstFactor) ;

        static void SubmitMesh(const std::shared_ptr<Mesh>& mesh);

        static void DrawIndexed(uint32_t count);
        static void DrawFrame();

        static RendererAPI::API GetAPI() { return RendererAPI::GetAPI(); }
        static RendererAPI* GetRendererAPI() { return s_RendererAPI.get(); }

    private:
        static std::unique_ptr<RendererAPI> s_RendererAPI;
    };
}
