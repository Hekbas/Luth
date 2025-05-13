#pragma once

#include "luth/core/LuthTypes.h"
#include "luth/core/Math.h"
#include "luth/renderer/Framebuffer.h"

#include <memory>

namespace Luth
{
    class Mesh;

    enum class BufferBit
    {
        None    = 0,
        Color   = 1 << 0,
        Depth   = 1 << 1,
        Stencil = 1 << 2
    };

    class RendererAPI
    {
    public:
        enum class API
        {
            None = 0,
            OpenGL,
            Vulkan //TODO :')
        };

        enum class RenderMode { Opaque, Cutout, Transparent, Fade };
        enum class BlendFactor { Zero, One, SrcAlpha, OneMinusSrcAlpha, DstAlpha, OneMinusDstAlpha };

        virtual ~RendererAPI() = default;

        virtual void Init() = 0;
        virtual void Shutdown() = 0;

        virtual void BindFramebuffer(const std::shared_ptr<Framebuffer>& framebuffer) = 0;

        virtual void SetViewport(u32 x, u32 y, u32 width, u32 height) = 0;
        virtual void SetClearColor(const glm::vec4& color) = 0;
        virtual void Clear(BufferBit bits) = 0;

        virtual void EnableDepthMask(bool enable) = 0;
        virtual bool IsDepthMaskEnabled() = 0;

        virtual void EnableDepthTest(bool enable) = 0;
        virtual bool IsDepthTestEnabled() const = 0;
         
        virtual void EnableBlending(bool enable) = 0;
        virtual void SetBlendFunction(BlendFactor srcFactor, BlendFactor dstFactor) = 0;

        virtual void SubmitMesh(const std::shared_ptr<Mesh>& mesh) = 0;

        virtual void DrawIndexed(u32 count) = 0;
        virtual void DrawFrame() = 0;

        virtual void InitFullscreenQuad() = 0;
        virtual void DrawFullscreenQuad() = 0;

        static API GetAPI() { return s_API; }
        static const char* APIToString(RendererAPI::API api);
        static void SetWindow(void* window);

        static std::unique_ptr<RendererAPI> Create(API api);

    protected:
        static API s_API;
        static inline void* s_Window = nullptr;
    };

    inline BufferBit operator|(BufferBit lhs, BufferBit rhs)
    {
        return static_cast<BufferBit>(
            static_cast<unsigned int>(lhs) | static_cast<unsigned int>(rhs));
    }
}
