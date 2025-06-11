#include "Luthpch.h"
#include "luth/renderer/pipeline/passes/LightingPass.h"
#include "luth/renderer/pipeline/passes/SSAOPass.h"
#include "luth/renderer/pipeline/passes/GeometryPass.h"
#include "luth/renderer/pipeline/RenderPipeline.h"

namespace Luth
{
    void LightingPass::Init(u32 w, u32 h)
    {
        m_LightShader = ShaderLibrary::Get("LuthDeferredLight");
        m_LightFBO = Framebuffer::Create({
            .Width = w, .Height = h,
            .ColorAttachments = {{.InternalFormat = GL_RGBA16F }}
        });
    }

    void LightingPass::Resize(u32 w, u32 h)
    {
        m_LightFBO->Resize(w, h);
    }

    void LightingPass::Execute(const RenderContext& ctx)
    {
        m_LightFBO->Bind();
        Renderer::Clear(BufferBit::Color);

        m_LightShader->Bind();
        auto geoFBO = ctx.pipeline->GetPass<GeometryPass>()->GetGBuffer();
        auto ssaoFBO = ctx.pipeline->GetPass<SSAOPass>()->GetGBuffer();

		geoFBO->BindColorAsTexture(0, 0);
        m_LightShader->SetInt("o_Position", 0);

        geoFBO->BindColorAsTexture(1, 1);
		m_LightShader->SetInt("o_Normal", 1);

        geoFBO->BindColorAsTexture(2, 2);
		m_LightShader->SetInt("o_Albedo", 2);

        geoFBO->BindColorAsTexture(3, 3);
        m_LightShader->SetInt("o_MRO", 3);

        geoFBO->BindColorAsTexture(4, 4);
        m_LightShader->SetInt("o_ET", 4);

		ssaoFBO->BindColorAsTexture(0, 5);
        m_LightShader->SetInt("o_SSAO", 5);

        Renderer::DrawFullscreenQuad();
        m_LightFBO->Unbind();
    }
}
