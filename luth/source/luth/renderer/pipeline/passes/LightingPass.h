#pragma once

#include "luth/renderer/pipeline/RenderPass.h"

namespace Luth
{
    class LightingPass : public RenderPass
    {
    public:
        void Init(u32 width, u32 height) override;
        void Resize(u32 width, u32 height) override;
        void Execute(const RenderContext& ctx) override;
        
        u32 GetFinalColorAttachment() const { return m_LightFBO->GetColorAttachmentID(); }

        std::vector<std::pair<std::string, u32>> GetAllAttachments() const {
            return {
              { "Light", m_LightFBO->GetColorAttachmentID() },
            };
        }

        std::shared_ptr<Framebuffer> GetGBuffer() const { return m_LightFBO; }
        
    private:
        std::shared_ptr<Framebuffer> m_LightFBO;
        std::shared_ptr<Shader> m_LightShader;
    };
}
