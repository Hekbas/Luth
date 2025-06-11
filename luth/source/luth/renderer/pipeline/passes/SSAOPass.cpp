#include "Luthpch.h"
#include "luth/renderer/pipeline/passes/SSAOPass.h"
#include "luth/renderer/pipeline/passes/GeometryPass.h"
#include "luth/renderer/pipeline/RenderPipeline.h"

namespace Luth
{
    void SSAOPass::Init(u32 w, u32 h)
    {
        m_SSAOShader     = ShaderLibrary::Get("LuthSSAO");
        m_SSAOBlurShader = ShaderLibrary::Get("LuthSSAOBlur");

        m_SSAOFBO     = Framebuffer::Create({ .Width = w, .Height = h, .ColorAttachments = {{.InternalFormat = GL_R8 }} });
        m_SSAOBlurFBO = Framebuffer::Create({ .Width = w, .Height = h, .ColorAttachments = {{.InternalFormat = GL_R8 }} });

        // build kernel & SSBO
        m_Kernel.resize(64);
        InitKernel();
        glGenBuffers(1, &m_KernelSSBO);
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, m_KernelSSBO);
        glBufferData(GL_SHADER_STORAGE_BUFFER, m_Kernel.size() * sizeof(glm::vec3), m_Kernel.data(), GL_STATIC_READ);
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 2, m_KernelSSBO);
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);

        // noise texture
        InitNoiseTexture();
    }

    void SSAOPass::Resize(u32 w, u32 h)
    {
        m_SSAOFBO->Resize(w, h);
        m_SSAOBlurFBO->Resize(w, h);
    }

    void SSAOPass::Execute(const RenderContext& ctx)
    {
        auto geoFBO = ctx.pipeline->GetPass<GeometryPass>()->GetGBuffer();

        // Bind G-buffer attachments
        geoFBO->BindColorAsTexture(0, 0);   // Position
        geoFBO->BindColorAsTexture(1, 1);   // Normal

        // Bind noise texture
        m_NoiseTexture->Bind(2);

        // Set up SSAO shader
        m_SSAOShader->Bind();
        m_SSAOShader->SetInt("gPosition", 0);
        m_SSAOShader->SetInt("gNormal",   1);
        m_SSAOShader->SetInt("u_Noise",   2);
        m_SSAOShader->SetVec2("u_NoiseScale", { ctx.width / 4, ctx.height / 4 });
        m_SSAOShader->SetFloat("u_Radius", m_Radius);
        m_SSAOShader->SetFloat("u_Bias", m_Bias);

        // Render into SSAO FBO
        m_SSAOFBO->Bind();
        Renderer::Clear(BufferBit::Color);
        Renderer::DrawFullscreenQuad();
        m_SSAOFBO->Unbind();

        // Blur
        m_SSAOBlurFBO->Bind();
        Renderer::Clear(BufferBit::Color);
        m_SSAOBlurShader->Bind();
        m_SSAOFBO->BindColorAsTexture(0, 0);
        Renderer::DrawFullscreenQuad();
        m_SSAOBlurFBO->Unbind();
    }

    void SSAOPass::SetKernelSize(u32 n)
    {
        m_Kernel.resize(n);
        InitKernel();
        // re-upload
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, m_KernelSSBO);
        glBufferData(GL_SHADER_STORAGE_BUFFER, m_Kernel.size() * sizeof(glm::vec3), m_Kernel.data(), GL_STATIC_READ);
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
    }

    void SSAOPass::InitKernel()
    {
        std::uniform_real_distribution<float> randomFloats(0.0f, 1.0f);
        std::default_random_engine generator;

        for (u32 i = 0; i < m_Kernel.size(); i++) {
            // generate a sample in the hemisphere
            glm::vec3 sample(
                randomFloats(generator) * 2.0f - 1.0f,
                randomFloats(generator) * 2.0f - 1.0f,
                randomFloats(generator)             // Z in [0,1] for hemisphere
            );
            sample = glm::normalize(sample);
            sample *= randomFloats(generator);

            // Scale so more points are near the origin
            float scale = float(i) / float(m_Kernel.size());
            scale = glm::mix(0.1f, 1.0f, scale * scale);
            m_Kernel[i] = sample * scale;
        }
    }

    void SSAOPass::InitNoiseTexture()
    {
        std::vector<glm::vec3> noise;
        noise.reserve(NOISE_SIZE * NOISE_SIZE);
        std::uniform_real_distribution<float> dist(-1.0f, 1.0f);
        std::default_random_engine gen;

        for (int i = 0; i < NOISE_SIZE * NOISE_SIZE; i++)
            noise.emplace_back(dist(gen) * 2.0 - 1.0, dist(gen) * 2.0 - 1.0, 0.0f);

        m_NoiseTexture = Texture::Create(
            NOISE_SIZE, NOISE_SIZE,
            TextureFormat::RGB8,
            reinterpret_cast<const float*>(noise.data())
        );
        m_NoiseTexture->SetWrapMode(TextureWrapMode::Repeat);
        m_NoiseTexture->SetFilterMode(TextureFilterMode::Nearest, TextureFilterMode::Nearest);
    }
}