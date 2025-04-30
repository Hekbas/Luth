#pragma once

#include "luth/renderer/RenderTechnique.h"
#include "luth/renderer/Material.h"

namespace Luth
{
    class ForwardTechnique : public RenderTechnique
    {
    public:
        ForwardTechnique() : RenderTechnique("Forward") { Init(1280, 720); }

        void Init(u32 width, u32 height) override;
        void Shutdown() override;

        void Render(entt::registry& registry,
            const Vec3& cameraPos,
            const std::vector<RenderCommand>& opaque,
            const std::vector<RenderCommand>& transparent) override;
        void Resize(u32 width, u32 height) override;

        u32 GetFinalColorAttachment() const { return m_MainFBO->GetColorAttachmentID(); }
        std::vector<std::pair<std::string, u32>> GetAllAttachments() const override;

    private:
        void RenderMesh(const RenderCommand& cmd, bool isOpaque);
        void BindMaterialTextures(const std::shared_ptr<Material>& material,
            const std::shared_ptr<Shader>& shader);

        std::shared_ptr<Framebuffer> m_MainFBO;
        glm::mat4 m_ViewProjection;
    };
}
