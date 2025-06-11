#pragma once

#include "luth/renderer/pipeline/RenderPass.h"

namespace Luth
{
    class TransparentPass : public RenderPass
    {
    public:
        void Init(u32 width, u32 height) override;
        void Resize(u32 width, u32 height) override;
        void Execute(const RenderContext& ctx) override;

        u32 GetFinalColorAttachment() const { return m_TransparentFBO->GetColorAttachmentID(); }

        std::vector<std::pair<std::string, u32>> GetAllAttachments() const {
            return {
              { "Transparent", m_TransparentFBO->GetColorAttachmentID() },
            };
        }

        std::shared_ptr<Framebuffer> GetGBuffer() const { return m_TransparentFBO; }
        
    private:
        std::shared_ptr<Framebuffer> m_TransparentFBO;
        std::shared_ptr<Shader> m_FLightShader;
    };
}
