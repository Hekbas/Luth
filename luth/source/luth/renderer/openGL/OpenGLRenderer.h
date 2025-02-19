#pragma once

#include "luth/renderer/Renderer.h"

namespace Luth
{
    class OpenGLRenderer : public Renderer
    {
    public:
        OpenGLRenderer() = default;

        void Init() override;
        void Shutdown() override;

        void SetClearColor(const glm::vec4& color) override;
        void Clear() override;
        void SetViewport(u32 x, u32 y, u32 width, u32 height) override;

        void EnableDepthTest(bool enable) override;
        bool IsDepthTestEnabled() const override { return m_DepthTestEnabled; }

        void EnableBlending(bool enable) override;
        void SetBlendFunction(u32 srcFactor, u32 dstFactor) override;

        void DrawIndexed(u32 count) override;

    private:
        void CheckError(const char* file, int line);

        bool m_DepthTestEnabled = true;
        bool m_BlendingEnabled = true;
        glm::vec4 m_ClearColor = { 0.1f, 0.1f, 0.1f, 1.0f };
    };
}
