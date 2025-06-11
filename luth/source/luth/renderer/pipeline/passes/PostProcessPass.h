#pragma once

#include "luth/renderer/pipeline/RenderPass.h"

namespace Luth
{
    enum class ToneMapOperator {
        LINEAR = 0,
        REINHARD,
        REINHARD_MODIFIED,
        ACES,
        FILMIC,
        UNCHARTED2
    };

    class PostProcessPass : public RenderPass
    {
    public:
        void Init(u32 width, u32 height) override;
        void Resize(u32 width, u32 height) override;
        void Execute(const RenderContext& ctx) override;

        u32 GetFinalColorAttachment() const { return m_OutputFBO->GetColorAttachmentID(); }

        std::vector<std::pair<std::string, u32>> GetAllAttachments() const {
            return {
              { "Final", m_OutputFBO->GetColorAttachmentID(0) },
            };
        }

        std::shared_ptr<Framebuffer> GetGBuffer() const { return m_OutputFBO; }

        // Bloom settings
        float GetBloomThreshold() const { return m_BloomThreshold; }
        void SetBloomThreshold(float t) { m_BloomThreshold = t; }

        float GetBloomStrength() const { return m_BloomStrength; }
        void SetBloomStrength(float s) { m_BloomStrength = s; }

        int GetBloomPasses() const { return m_BloomPasses; }
        void SetBloomPasses(int p) { m_BloomPasses = p; }

        // Grain
        float GetGrainAmount() const { return m_GrainAmount; }
        void SetGrainAmount(float g) { m_GrainAmount = g; }

        // Sharpness
        float GetSharpness() const { return m_Sharpness; }
        void SetSharpness(float s) { m_Sharpness = s; }

        // Chromatic Aberration
        float GetAberrationOffset() const { return m_AberrationOffset; }
        void SetAberrationOffset(float o) { m_AberrationOffset = o; }

        // Vignette
        float GetVignetteAmount() const { return m_VignetteAmount; }
        void SetVignetteAmount(float a) { m_VignetteAmount = a; }

        float GetVignetteHardness() const { return m_VignetteHardness; }
        void SetVignetteHardness(float h) { m_VignetteHardness = h; }

        // Tone mapping controls
        ToneMapOperator GetToneMapOperator() const { return m_ToneMapOp; }
        void SetToneMapOperator(ToneMapOperator op) { m_ToneMapOp = op; }

        float GetExposure() const { return m_Exposure; }
        void SetExposure(float e) { m_Exposure = e; }

        float GetContrast() const { return m_Contrast; }
        void SetContrast(float c) { m_Contrast = c; }

        float GetSaturation() const { return m_Saturation; }
        void SetSaturation(float s) { m_Saturation = s; }

        // Color balance (shadows / mids / highlights)
        const glm::vec3& GetShadowBalance() const { return m_ShadowBalance; }
        const glm::vec3& GetMidtoneBalance() const { return m_MidtoneBalance; }
        const glm::vec3& GetHighlightBalance() const { return m_HighlightBalance; }
        void SetColorBalance(const glm::vec3& shadows, const glm::vec3& mids, const glm::vec3& highlights) {
            m_ShadowBalance = shadows;
            m_MidtoneBalance = mids;
            m_HighlightBalance = highlights;
        }
        
    private:
        // Intermediate bloom FBOs
        std::shared_ptr<Framebuffer> m_BloomExtractFBO;
        std::shared_ptr<Framebuffer> m_PingPongFBO[2];

        // Final composite FBO
        std::shared_ptr<Framebuffer> m_OutputFBO;

        // Shaders
        std::shared_ptr<Shader> m_BloomExtractShader;
        std::shared_ptr<Shader> m_BloomBlurShader;
        std::shared_ptr<Shader> m_PostProcessShader;

        // Bloom
        float m_BloomThreshold = 1.0f;
        float m_BloomStrength = 1.0f;
        int   m_BloomPasses = 8;

        // Others
        float m_GrainAmount = 0.03f;
        float m_Sharpness = 0.8f;
        float m_AberrationOffset = 0.002f;
        float m_VignetteAmount = 0.5f;
        float m_VignetteHardness = 0.25f;

        // Color Balance
        glm::vec3 m_ShadowBalance = { 1.0f, 1.0f, 1.0f };
        glm::vec3 m_MidtoneBalance = { 1.0f, 1.0f, 1.0f };
        glm::vec3 m_HighlightBalance = { 1.0f, 1.0f, 1.0f };

        // Tone mapping
        ToneMapOperator m_ToneMapOp = ToneMapOperator::ACES;
        float m_Exposure = 1.0f;
        float m_Contrast = 2.0f;
        float m_Saturation = 1.0f;
    };
}
