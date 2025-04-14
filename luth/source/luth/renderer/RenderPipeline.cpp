#include "Luthpch.h"
#include "luth/renderer/RenderPipeline.h"

namespace Luth
{
    void RenderPipeline::AddPass(const RenderPass& pass)
    {
        m_Passes.push_back(pass);
        if (!pass.Name.empty()) {
            m_NamedFramebuffers[pass.Name] = pass.TargetFBO;
        }
    }

    void RenderPipeline::Execute()
    {
        for (auto& pass : m_Passes) {
            if (pass.TargetFBO) {
                pass.TargetFBO->Bind();
                glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
            }
            else {
                Framebuffer::Unbind();
            }

            // Bind input attachments
            for (size_t i = 0; i < pass.InputAttachments.size(); ++i) {
                if (auto fbo = GetFramebuffer(pass.InputAttachments[i])) {
                    fbo->BindColorAsTexture(i, i);
                }
            }

            pass.Execute();
        }
    }

    void RenderPipeline::Resize(uint32_t width, uint32_t height)
    {
        for (auto& [name, fbo] : m_NamedFramebuffers) {
            fbo->Resize(width, height);
        }
    }
}
