#pragma once

#include "luth/renderer/Renderer.h"

namespace Luth
{
    class GLRendererAPI : public RendererAPI
    {
    public:
        virtual void Init() override;
        virtual void Shutdown() override;

        virtual void SetViewport(u32 x, u32 y, u32 width, u32 height) override;
        virtual void SetClearColor(const glm::vec4& color) override;
        virtual void Clear() override;

        void EnableDepthTest(bool enable);
        bool IsDepthTestEnabled() const { return m_DepthTestEnabled; }

        void EnableBlending(bool enable);
        void SetBlendFunction(u32 srcFactor, u32 dstFactor);
        
        virtual void DrawIndexed(u32 count) override;
        virtual void DrawFrame() override;

    private:
        void CheckError(const char* file, int line);

        bool m_DepthTestEnabled = true;
        bool m_BlendingEnabled = true;
        glm::vec4 m_ClearColor = { 0.1f, 0.1f, 0.1f, 1.0f };
    };
}
