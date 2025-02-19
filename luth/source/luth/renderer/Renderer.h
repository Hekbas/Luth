#pragma once

#include "luth/core/LuthTypes.h"

#include <glm/glm.hpp>
#include <memory>

namespace Luth
{
    enum class RendererAPI
    {
        None = 0,
        OpenGL,
        Vulkan //TODO :')
    };

    class Renderer
    {
    public:
        virtual ~Renderer() = default;

        virtual void Init() = 0;
        virtual void Shutdown() = 0;

        virtual void SetClearColor(const glm::vec4& color) = 0;
        virtual void Clear() = 0;
        virtual void SetViewport(u32 x, u32 y, u32 width, u32 height) = 0;

        virtual void EnableDepthTest(bool enable) = 0;
        virtual bool IsDepthTestEnabled() const = 0;

        virtual void EnableBlending(bool enable) = 0;
        virtual void SetBlendFunction(u32 srcFactor, u32 dstFactor) = 0;

        virtual void DrawIndexed(u32 count) = 0;

        static RendererAPI GetAPI();
        static std::unique_ptr<Renderer> Create();

    protected:
        static RendererAPI s_API;
    };
}
