#include "Luthpch.h"
#include "luth/renderer/pipeline/passes/PostProcessPass.h"
#include "luth/renderer/pipeline/passes/LightingPass.h"
#include "luth/renderer/pipeline/RenderPipeline.h"

namespace Luth
{
    void PostProcessPass::Init(u32 width, u32 height)
    {
        // Load shaders
        m_BloomExtractShader = ShaderLibrary::Get("LuthBloomExtract");
        m_BloomBlurShader = ShaderLibrary::Get("LuthBloomBlur");
        m_PostProcessShader = ShaderLibrary::Get("LuthPostProcess");

        // Bloom extract at full resolution
        m_BloomExtractFBO = Framebuffer::Create({
            .Width = width,
            .Height = height,
            .ColorAttachments = {{.InternalFormat = GL_RGB16F }}
        });

        // Ping-pong blur buffers at half resolution
        Framebuffer::Spec half = {
            .Width = width / 2,
            .Height = height / 2,
            .ColorAttachments = {{.InternalFormat = GL_RGB16F }}
        };
        m_PingPongFBO[0] = Framebuffer::Create(half);
        m_PingPongFBO[1] = Framebuffer::Create(half);

        // Final output
        m_OutputFBO = Framebuffer::Create({
            .Width = width,
            .Height = height,
            .ColorAttachments = {{.InternalFormat = GL_RGBA16F }}
        });
    }

    void PostProcessPass::Resize(u32 width, u32 height)
    {
        m_BloomExtractFBO->Resize(width, height);
        m_PingPongFBO[0]->Resize(width / 2, height / 2);
        m_PingPongFBO[1]->Resize(width / 2, height / 2);
        m_OutputFBO->Resize(width, height);
    }

    void PostProcessPass::Execute(const RenderContext& ctx)
    {
        auto lightFBO = ctx.pipeline->GetPass<TransparentPass>()->GetGBuffer();

        // 1) Brightness extract
        m_BloomExtractShader->Bind();
        m_BloomExtractShader->SetFloat("u_Threshold", m_BloomThreshold);

        lightFBO->BindColorAsTexture(0, 0);
        m_BloomExtractShader->SetInt("u_Scene", 0);

        m_BloomExtractFBO->Bind();
        Renderer::Clear(BufferBit::Color);
        Renderer::DrawFullscreenQuad();
        m_BloomExtractFBO->Unbind();

        // 2) Gaussian blur ping-pong
        bool horizontal = true;
        bool firstIteration = true;
        const float blurStrength = 2.0f; // Adjust based on resolution

        for (int i = 0; i < m_BloomPasses * 2; i++)
        {
            m_PingPongFBO[horizontal]->Bind();
            Renderer::Clear(BufferBit::Color);
            m_BloomBlurShader->Bind();

            // Bind input texture
            if (firstIteration) {
                m_BloomExtractFBO->BindColorAsTexture(0, 0);
                firstIteration = false;
            }
            else {
                m_PingPongFBO[!horizontal]->BindColorAsTexture(0, 0);
            }

            // Set blur parameters
            m_BloomBlurShader->SetFloat("u_Horizontal", horizontal);
            m_BloomBlurShader->SetFloat("u_BlurStrength", m_BloomStrength);

            Renderer::DrawFullscreenQuad();
            m_PingPongFBO[horizontal]->Unbind();

            horizontal = !horizontal;
        }

        // 3) Final composite
        m_PostProcessShader->Bind();
        // bind inputs
        lightFBO->BindColorAsTexture(0, 0);
        m_PostProcessShader->SetInt("u_Scene", 0);
        m_PingPongFBO[0]->BindColorAsTexture(0, 1);
        m_PostProcessShader->SetInt("u_Bloom", 1);

        m_PostProcessShader->SetFloat("u_Time", Time::GetTime());
        m_PostProcessShader->SetFloat("u_BloomStrength", m_BloomStrength);

        m_PostProcessShader->SetFloat("u_GrainAmount", m_GrainAmount);
        m_PostProcessShader->SetFloat("u_Sharpness", m_Sharpness);
        m_PostProcessShader->SetFloat("u_AberrationOffset", m_AberrationOffset);
        m_PostProcessShader->SetFloat("u_VignetteAmount", m_VignetteAmount);
        m_PostProcessShader->SetFloat("u_VignetteHardness", m_VignetteHardness);
        
        m_PostProcessShader->SetInt("u_ToneMapOperator", static_cast<int>(m_ToneMapOp));
        m_PostProcessShader->SetFloat("u_Exposure", m_Exposure);
        m_PostProcessShader->SetFloat("u_Contrast", m_Contrast);
        m_PostProcessShader->SetFloat("u_Saturation", m_Saturation);

        m_PostProcessShader->SetVec3("u_ShadowBalance", m_ShadowBalance);
        m_PostProcessShader->SetVec3("u_MidtoneBalance", m_MidtoneBalance);
        m_PostProcessShader->SetVec3("u_HighlightBalance", m_HighlightBalance);

        m_OutputFBO->Bind();
        Renderer::DrawFullscreenQuad();
        m_OutputFBO->Unbind();
    }
}
