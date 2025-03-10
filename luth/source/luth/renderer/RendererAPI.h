#pragma once

#include "luth/core/LuthTypes.h"
#include "luth/renderer/Mesh.h"

#include <glm/glm.hpp>
#include <memory>

namespace Luth
{
    class RendererAPI
    {
    public:
        enum class API
        {
            None = 0,
            OpenGL,
            Vulkan //TODO :')
        };

        virtual ~RendererAPI() = default;

        virtual void Init() = 0;
        virtual void Shutdown() = 0;

        virtual void SetViewport(u32 x, u32 y, u32 width, u32 height) = 0;
        virtual void SetClearColor(const glm::vec4& color) = 0;
        virtual void Clear() = 0;

        //virtual void EnableDepthTest(bool enable) = 0;
        //virtual bool IsDepthTestEnabled() const = 0;
        // 
        //virtual void EnableBlending(bool enable) = 0;
        //virtual void SetBlendFunction(u32 srcFactor, u32 dstFactor) = 0;

        virtual void SubmitMesh(const std::shared_ptr<Mesh>& mesh) = 0;

        virtual void DrawIndexed(u32 count) = 0;
        virtual void DrawFrame() = 0;

        static API GetAPI() { return s_API; }
        static const char* APIToString(RendererAPI::API api);
        static void SetWindow(void* window);

        static std::unique_ptr<RendererAPI> Create(API api);

    protected:
        static API s_API;
        static inline void* s_Window = nullptr;
    };
}
