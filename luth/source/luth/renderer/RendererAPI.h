#pragma once

#include "luth/core/LuthTypes.h"

#include <memory>

namespace Luth
{
    namespace RG { class RenderGraph; }

    class RendererAPI
    {
    public:
        enum class API
        {
            None = 0,
            Vulkan
        };

        virtual ~RendererAPI() = default;

        virtual void Init(void* windowHandle) = 0;
        virtual void Shutdown() = 0;

        virtual bool BeginFrame() = 0;
        virtual void EndFrame() = 0;
        
        // The main entry point for rendering a frame
        virtual void ExecuteGraph(RG::RenderGraph& graph) = 0;

        // Handle window resize events
        virtual void OnResize(u32 width, u32 height) = 0;

        static API GetAPI() { return s_API; }
        static std::unique_ptr<RendererAPI> Create();

    protected:
        static API s_API;
    };
}
