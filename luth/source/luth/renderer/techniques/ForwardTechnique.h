#pragma once

#include "luth/renderer/RenderTechnique.h"
#include "luth/renderer/Material.h"

namespace Luth
{
    class ForwardTechnique : public RenderTechnique
    {
    public:
        ForwardTechnique();

        void Init(u32 width, u32 height) override;
        void Shutdown() override;
        void Render(entt::registry& registry,
            const Vec3& cameraPos,
            const std::vector<RenderCommand>& opaque,
            const std::vector<RenderCommand>& transparent) override;
        void Resize(u32 width, u32 height) override;

        u32 GetFinalColorAttachment() const override;
        std::vector<std::pair<std::string, u32>> GetAllAttachments() const override;

    private:
        void RenderGeometryPrepass(const std::vector<RenderCommand>& commands);
        void RenderForwardPass(const std::vector<RenderCommand>& commands, const Vec3& cameraPos);
        void RenderMesh(const RenderCommand& cmd, Shader& shader);
        void BindMaterialTextures(const std::shared_ptr<Material>& material,
            const std::shared_ptr<Shader>& shader);
        void RenderSSAOPass();
        void RenderBloomPass();
        void RenderCompositePass();
        void InitSSAOKernel();
        void InitNoiseTexture();

        // Framebuffers
        std::shared_ptr<Framebuffer> m_GeometryFBO;
        std::shared_ptr<Framebuffer> m_MainFBO;
        std::shared_ptr<Framebuffer> m_SSAOFBO;
        std::shared_ptr<Framebuffer> m_SSAOBlurFBO;
        std::shared_ptr<Framebuffer> m_BrightnessFBO;
        std::array<std::shared_ptr<Framebuffer>, 2> m_PingPongFBO;
        std::shared_ptr<Framebuffer> m_CompositeFBO;

        // Shaders
        std::shared_ptr<Shader> m_GeoShader;
        std::shared_ptr<Shader> m_SSAOShader;
        std::shared_ptr<Shader> m_SSAOBlurShader;
        std::shared_ptr<Shader> m_BloomExtShader;
        std::shared_ptr<Shader> m_BloomBlurShader;
        std::shared_ptr<Shader> m_CompositeShader;

        // SSAO resources
        u32 m_SSBOKernel;
        std::vector<Vec3> m_SSAOKernel;
        std::shared_ptr<Texture> m_NoiseTexture;

        // Configuration
        u32 m_Width, m_Height;
        float m_SSAORadius = 0.5f;
        float m_SSAOBias = 0.025f;
        float m_BloomThreshold = 1.0f;
        int m_BloomBlurPasses = 8;
    };
}
