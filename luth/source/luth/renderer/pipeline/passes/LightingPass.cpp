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

        auto lightShader = m_LightShader.lock();

        if (!lightShader) {
            m_LightShader = ShaderLibrary::Get("LuthDeferredLight");
            lightShader = m_LightShader.lock();
        }

        lightShader->Bind();
        auto geoFBO = ctx.pipeline->GetPass<GeometryPass>()->GetGBuffer();
        auto ssaoFBO = ctx.pipeline->GetPass<SSAOPass>()->GetGBuffer();

		geoFBO->BindColorAsTexture(0, 0);
        lightShader->SetInt("o_Position", 0);

        geoFBO->BindColorAsTexture(1, 1);
        lightShader->SetInt("o_Normal", 1);

        geoFBO->BindColorAsTexture(2, 2);
        lightShader->SetInt("o_Albedo", 2);

        geoFBO->BindColorAsTexture(3, 3);
        lightShader->SetInt("o_MRO", 3);

        geoFBO->BindColorAsTexture(4, 4);
        lightShader->SetInt("o_ET", 4);

		ssaoFBO->BindColorAsTexture(0, 5);
        lightShader->SetInt("o_SSAO", 5);

        Renderer::DrawFullscreenQuad();
        m_LightFBO->Unbind();
    }
}
