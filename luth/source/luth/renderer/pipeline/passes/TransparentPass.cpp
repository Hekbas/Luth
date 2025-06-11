#include "Luthpch.h"
#include "luth/renderer/pipeline/passes/TransparentPass.h"
#include "luth/renderer/pipeline/passes/LightingPass.h"
#include "luth/renderer/pipeline/RenderPipeline.h"
#include "luth/renderer/pipeline/RenderUtils.h"

namespace Luth
{
    void TransparentPass::Init(u32 w, u32 h)
    {
        m_FLightShader = ShaderLibrary::Get("LuthForwardLight");
        m_TransparentFBO = Framebuffer::Create({
            .Width = w, .Height = h,
            .ColorAttachments = {
                { .InternalFormat = GL_RGBA16F },
            },
            .DepthStencilAttachment = {{.InternalFormat = GL_DEPTH24_STENCIL8 }}
        });
    }

    void TransparentPass::Resize(u32 width, u32 height)
    {
        m_TransparentFBO->Resize(width, height);
    }

    void TransparentPass::Execute(const RenderContext& ctx)
    {
        m_TransparentFBO->Bind();
        Renderer::Clear(BufferBit::Color | BufferBit::Depth);

        auto geoFBO = ctx.pipeline->GetPass<GeometryPass>()->GetGBuffer();
        auto lightFBO = ctx.pipeline->GetPass<LightingPass>()->GetGBuffer();
        auto width = lightFBO->GetSpecification().Width;
		auto height = lightFBO->GetSpecification().Height;

        // Blit color + depth
        glBindFramebuffer(GL_READ_FRAMEBUFFER, lightFBO->GetRendererID());
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, m_TransparentFBO->GetRendererID());
        glBlitFramebuffer(0, 0, width, height, 0, 0, width, height, GL_COLOR_BUFFER_BIT, GL_NEAREST);
        glBindFramebuffer(GL_READ_FRAMEBUFFER, geoFBO->GetRendererID());
        glBlitFramebuffer(0, 0, width, height, 0, 0, width, height, GL_DEPTH_BUFFER_BIT, GL_NEAREST);

        m_TransparentFBO->Bind();
        Renderer::EnableBlending(true);
        Renderer::EnableDepthMask(false);
		m_FLightShader->Bind();
        for (auto& cmd : ctx.transparent) {
            m_FLightShader->SetBool("u_IsSkinned", cmd.meshRend->isSkinned);
            RenderUtils::DrawCommand(cmd, *m_FLightShader);
        }
        Renderer::EnableBlending(false);
        Renderer::EnableDepthMask(true);

        m_TransparentFBO->Unbind();
    }
}
