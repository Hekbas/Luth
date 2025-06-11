#pragma once

#include "luth/renderer/pipeline/RenderPass.h"

namespace Luth
{
    class GeometryPass : public RenderPass
    {
    public:
        void Init(u32 width, u32 height) override;
        void Resize(u32 width, u32 height) override;
        void Execute(const RenderContext& ctx) override;

        u32 GetFinalColorAttachment() const { return m_GeoFBO->GetColorAttachmentID(); }

        std::vector<std::pair<std::string, u32>> GetAllAttachments() const {
            return {
			  { "Position",        m_GeoFBO->GetColorAttachmentID(0) },
			  { "Normal",          m_GeoFBO->GetColorAttachmentID(1) },
			  { "Base Color",      m_GeoFBO->GetColorAttachmentID(2) },
			  { "MRAO",            m_GeoFBO->GetColorAttachmentID(3) },
			  { "ET",              m_GeoFBO->GetColorAttachmentID(4) },
			  { "Bones Influence", m_GeoFBO->GetColorAttachmentID(5) }
            };
        }

        std::shared_ptr<Framebuffer> GetGBuffer() const { return m_GeoFBO; }
        
	private:
        std::shared_ptr<Framebuffer> m_GeoFBO;
        std::shared_ptr<Shader> m_GeoShader;
    };
}
