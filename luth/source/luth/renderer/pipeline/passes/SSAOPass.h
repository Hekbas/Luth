#pragma once

#include "luth/renderer/pipeline/RenderPass.h"

namespace Luth
{
    class SSAOPass : public RenderPass
    {
    public:
        void Init(u32 width, u32 height) override;
        void Resize(u32 width, u32 height) override;
        void Execute(const RenderContext& ctx) override;
        
        u32 GetFinalColorAttachment() const { return m_SSAOBlurFBO->GetColorAttachmentID(); }

        std::vector<std::pair<std::string, u32>> GetAllAttachments() const {
            return {
              { "SSAO",     m_SSAOFBO->GetColorAttachmentID()     },
              { "SSAOBlur", m_SSAOBlurFBO->GetColorAttachmentID() }
            };
        }

        std::shared_ptr<Framebuffer> GetGBuffer() const { return m_SSAOBlurFBO; }

		float GetRadius() const { return m_Radius; }
        void SetRadius(float r) { m_Radius = r; }

		float GetIntensity() const { return m_Intensity; }
		void SetIntensity(float i) { m_Intensity = i; }
        
		float GetBias() const { return m_Bias; }
        void SetBias(float b) { m_Bias = b; }

        void SetKernelSize(u32 n);

    private:
        void InitKernel();
        void InitNoiseTexture();

        std::shared_ptr<Framebuffer> m_SSAOFBO, m_SSAOBlurFBO;
        std::shared_ptr<Shader> m_SSAOShader, m_SSAOBlurShader;

        // SSAO parameters
        float m_Radius = 0.5f;
        float m_Intensity = 0.5f;
        float m_Bias = 0.025f;

        // Kernel storage
        std::vector<glm::vec3> m_Kernel;
        u32 m_KernelSSBO = 0;

        // Noise texture
        std::shared_ptr<Texture> m_NoiseTexture;
        static constexpr int NOISE_SIZE = 4;
    };
}
