#pragma once

#include "luth/renderer/RenderTechnique.h"

namespace Luth
{
    class Material;
    class Shader;

    class DeferredTechnique : public RenderTechnique
    {
    public:
        DeferredTechnique() : RenderTechnique("Deferred") { Init(1280, 720); }

        void Init(uint32_t width, uint32_t height) override;
        void Shutdown() override;

        void Render(entt::registry& registry,
            const glm::vec3& cameraPos,
            const std::vector<RenderCommand>& opaque,
            const std::vector<RenderCommand>& transparent) override;
        void Resize(uint32_t width, uint32_t height) override;
        void SetViewProjection(const glm::mat4& vp);
        uint32_t GetFinalColorAttachment() const override;

    private:
        enum GBufferColorIndex {
            Position = 0,
            Normal,
            Albedo,
            MRAO
        };

        void GeometryPass(const std::vector<RenderCommand>& commands);
        void LightingPass(entt::registry& registry);
        void CreateGBuffer();
        void CreateLightBuffer();

        void SetupMaterialTextures(const Material& material, Shader& shader);

        std::shared_ptr<Framebuffer> m_GBuffer;
        std::shared_ptr<Framebuffer> m_LightBuffer;
        std::shared_ptr<Shader> m_GeometryShader;
        std::shared_ptr<Shader> m_LightingShader;
        glm::mat4 m_ViewProjection;
    };
}
