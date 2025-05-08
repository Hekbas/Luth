#pragma once

#include "luth/renderer/Renderer.h"
#include "luth/renderer/Framebuffer.h"
#include "luth/renderer/Mesh.h"
#include "luth/renderer/openGL/GLMesh.h"

#include <memory>

namespace Luth
{
    class GLRendererAPI : public RendererAPI
    {
    public:
        virtual void Init() override;
        virtual void Shutdown() override;

        virtual void BindFramebuffer(const std::shared_ptr<Framebuffer>& framebuffer) override;

        virtual void SetViewport(u32 x, u32 y, u32 width, u32 height) override;
        virtual void SetClearColor(const glm::vec4& color) override;
        virtual void Clear(BufferBit bits) override;

        virtual void EnableDepthMask(bool enable) override;
        virtual bool IsDepthMaskEnabled() override;

        virtual void EnableDepthTest(bool enable);
        virtual bool IsDepthTestEnabled() const { return m_DepthTestEnabled; }

        virtual void EnableBlending(bool enable);
        virtual void SetBlendFunction(BlendFactor srcFactor, BlendFactor dstFactor);
        
        virtual void SubmitMesh(const std::shared_ptr<Mesh>& mesh) override;

        virtual void DrawIndexed(u32 count) override;
        virtual void DrawFrame() override;

        virtual void InitFullscreenQuad() override;
        virtual void DrawFullscreenQuad() override;

    private:

        GLenum BlendFactorToGL(BlendFactor factor) const;
        void CheckError(const char* file, int line);

        bool m_DepthTestEnabled = true;
        bool m_BlendingEnabled = true;
        glm::vec4 m_ClearColor = { 0.1f, 0.1f, 0.1f, 1.0f };

        std::vector<std::shared_ptr<GLMesh>> m_Meshes;

        GLuint quadVAO = 0;
        GLuint quadVBO = 0;
    };
}
