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

        auto bloomExtractShader = m_BloomExtractShader.lock();
        auto bloomBlurShader = m_BloomBlurShader.lock();
        auto postProcessShader = m_PostProcessShader.lock();

        if (!bloomExtractShader) {
            m_BloomExtractShader = ShaderLibrary::Get("LuthBloomExtract");
            bloomExtractShader = m_BloomExtractShader.lock();
        }
        if (!bloomBlurShader) {
            m_BloomBlurShader = ShaderLibrary::Get("LuthBloomBlur");
            bloomBlurShader = m_BloomBlurShader.lock();
        }
        if (!postProcessShader) {
            m_PostProcessShader = ShaderLibrary::Get("LuthPostProcess");
            postProcessShader = m_PostProcessShader.lock();
        }

        // 1) Brightness extract
        bloomExtractShader->Bind();
        bloomExtractShader->SetFloat("u_Threshold", m_BloomThreshold);

        lightFBO->BindColorAsTexture(0, 0);
        bloomExtractShader->SetInt("u_Scene", 0);

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
            bloomBlurShader->Bind();

            // Bind input texture
            if (firstIteration) {
                m_BloomExtractFBO->BindColorAsTexture(0, 0);
                firstIteration = false;
            }
            else {
                m_PingPongFBO[!horizontal]->BindColorAsTexture(0, 0);
            }

            // Set blur parameters
            bloomBlurShader->SetFloat("u_Horizontal", horizontal);
            bloomBlurShader->SetFloat("u_BlurStrength", m_BloomStrength);

            Renderer::DrawFullscreenQuad();
            m_PingPongFBO[horizontal]->Unbind();

            horizontal = !horizontal;
        }

        // 3) Final composite
        postProcessShader->Bind();
        // bind inputs
        lightFBO->BindColorAsTexture(0, 0);
        postProcessShader->SetInt("u_Scene", 0);
        m_PingPongFBO[0]->BindColorAsTexture(0, 1);
        postProcessShader->SetInt("u_Bloom", 1);

        postProcessShader->SetFloat("u_Time", Time::GetTime());
        postProcessShader->SetFloat("u_BloomStrength", m_BloomStrength);

        postProcessShader->SetFloat("u_GrainAmount", m_GrainAmount);
        postProcessShader->SetFloat("u_Sharpness", m_Sharpness);
        postProcessShader->SetFloat("u_AberrationOffset", m_AberrationOffset);
        postProcessShader->SetFloat("u_VignetteAmount", m_VignetteAmount);
        postProcessShader->SetFloat("u_VignetteHardness", m_VignetteHardness);
        
        postProcessShader->SetInt("u_ToneMapOperator", static_cast<int>(m_ToneMapOp));
        postProcessShader->SetFloat("u_Exposure", m_Exposure);
        postProcessShader->SetFloat("u_Contrast", m_Contrast);
        postProcessShader->SetFloat("u_Saturation", m_Saturation);

        postProcessShader->SetVec3("u_ShadowBalance", m_ShadowBalance);
        postProcessShader->SetVec3("u_MidtoneBalance", m_MidtoneBalance);
        postProcessShader->SetVec3("u_HighlightBalance", m_HighlightBalance);

        m_OutputFBO->Bind();
        Renderer::DrawFullscreenQuad();
        m_OutputFBO->Unbind();
    }
}
