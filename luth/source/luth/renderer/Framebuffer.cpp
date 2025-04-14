#include "luthpch.h"
#include "luth/renderer/Framebuffer.h"

#include <glad/glad.h>

namespace Luth
{
    std::shared_ptr<Framebuffer> Framebuffer::Create(const Spec& spec)
    {
        return std::make_shared<Framebuffer>(spec);
    }

    Framebuffer::Framebuffer(const Spec& spec) : m_Spec(spec)
    {
        Invalidate();
    }

    Framebuffer::~Framebuffer()
    {
        DeleteAttachments();
        glDeleteFramebuffers(1, &m_RendererID);
    }

    void Framebuffer::Bind()
    {
        glBindFramebuffer(GL_FRAMEBUFFER, m_RendererID);
        glViewport(0, 0, m_Spec.Width, m_Spec.Height);
    }

    void Framebuffer::Unbind()
    {
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
    }

    void Framebuffer::Resize(u32 width, u32 height)
    {
        if (width == 0 || height == 0) {
            LH_CORE_WARN("Attempted to resize framebuffer to {0}x{1}", width, height);
            return;
        }

        m_Spec.Width = width;
        m_Spec.Height = height;
        Invalidate();
    }

    void Framebuffer::BindColorAsTexture(u32 index, u32 slot) const
    {
        if (index >= m_ColorAttachments.size()) {
            LH_CORE_ERROR("Framebuffer color attachment index out of range!");
            return;
        }
        glBindTextureUnit(slot, m_ColorAttachments[index]);
    }

    void Framebuffer::BindDepthAsTexture(u32 slot) const
    {
        if (m_DepthAttachment == 0) {
            LH_CORE_ERROR("No depth/stencil attachment exists!");
            return;
        }
        if (!m_Spec.DepthStencilAttachment.has_value() || !m_Spec.DepthStencilAttachment->IsTexture) {
            LH_CORE_ERROR("Depth/stencil attachment is not a texture!");
            return;
        }
        glBindTextureUnit(slot, m_DepthAttachment);
    }

    u32 Framebuffer::GetColorAttachmentID(u32 index) const
    {
        if (index >= m_ColorAttachments.size()) {
            LH_CORE_ERROR("Framebuffer color attachment index out of range!");
            return 0;
        }
        return m_ColorAttachments[index];
    }

    u32 Framebuffer::GetDepthAttachmentID() const
    {
        if (m_DepthAttachment == 0 || (m_Spec.DepthStencilAttachment.has_value() && !m_Spec.DepthStencilAttachment->IsTexture)) {
            LH_CORE_ERROR("Depth/stencil attachment is not a texture!");
            return 0;
        }
        return m_DepthAttachment;
    }

    void Framebuffer::Invalidate()
    {
        if (m_RendererID) {
            DeleteAttachments();
            glDeleteFramebuffers(1, &m_RendererID);
        }

        glCreateFramebuffers(1, &m_RendererID);
        glBindFramebuffer(GL_FRAMEBUFFER, m_RendererID);

        // Color attachments
        std::vector<GLenum> drawBuffers;
        m_ColorAttachments.clear();
        m_ColorAttachments.reserve(m_Spec.ColorAttachments.size());

        for (size_t i = 0; i < m_Spec.ColorAttachments.size(); i++) {
            const auto& attSpec = m_Spec.ColorAttachments[i];
            u32 id;

            if (attSpec.IsTexture) {
                GLenum target = m_Spec.Samples > 1 ? GL_TEXTURE_2D_MULTISAMPLE : GL_TEXTURE_2D;
                glCreateTextures(target, 1, &id);

                if (m_Spec.Samples > 1) {
                    glTextureStorage2DMultisample(id, m_Spec.Samples, attSpec.InternalFormat,
                        m_Spec.Width, m_Spec.Height, GL_TRUE);
                }
                else {
                    glTextureStorage2D(id, 1, attSpec.InternalFormat, m_Spec.Width, m_Spec.Height);
                    glTextureParameteri(id, GL_TEXTURE_MIN_FILTER, attSpec.MinFilter);
                    glTextureParameteri(id, GL_TEXTURE_MAG_FILTER, attSpec.MagFilter);
                    glTextureParameteri(id, GL_TEXTURE_WRAP_S, attSpec.WrapS);
                    glTextureParameteri(id, GL_TEXTURE_WRAP_T, attSpec.WrapT);
                }

                glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0 + i, target, id, 0);
            }
            else {
                glCreateRenderbuffers(1, &id);
                if (m_Spec.Samples > 1) {
                    glNamedRenderbufferStorageMultisample(id, m_Spec.Samples, attSpec.InternalFormat,
                        m_Spec.Width, m_Spec.Height);
                }
                else {
                    glNamedRenderbufferStorage(id, attSpec.InternalFormat, m_Spec.Width, m_Spec.Height);
                }
                glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0 + i, GL_RENDERBUFFER, id);
            }

            m_ColorAttachments.push_back(id);
            drawBuffers.push_back(GL_COLOR_ATTACHMENT0 + i);
        }

        // Depth/Stencil attachment
        m_DepthAttachment = 0;
        if (m_Spec.DepthStencilAttachment.has_value()) {
            const auto& depthSpec = m_Spec.DepthStencilAttachment.value();
            GLenum attachmentPoint = GetAttachmentPoint(depthSpec.InternalFormat);

            if (depthSpec.IsTexture) {
                GLenum target = m_Spec.Samples > 1 ? GL_TEXTURE_2D_MULTISAMPLE : GL_TEXTURE_2D;
                glCreateTextures(target, 1, &m_DepthAttachment);

                if (m_Spec.Samples > 1) {
                    glTextureStorage2DMultisample(m_DepthAttachment, m_Spec.Samples,
                        depthSpec.InternalFormat, m_Spec.Width, m_Spec.Height, GL_TRUE);
                }
                else {
                    glTextureStorage2D(m_DepthAttachment, 1, depthSpec.InternalFormat,
                        m_Spec.Width, m_Spec.Height);
                    glTextureParameteri(m_DepthAttachment, GL_TEXTURE_MIN_FILTER, depthSpec.MinFilter);
                    glTextureParameteri(m_DepthAttachment, GL_TEXTURE_MAG_FILTER, depthSpec.MagFilter);
                    glTextureParameteri(m_DepthAttachment, GL_TEXTURE_WRAP_S, depthSpec.WrapS);
                    glTextureParameteri(m_DepthAttachment, GL_TEXTURE_WRAP_T, depthSpec.WrapT);
                }

                glFramebufferTexture2D(GL_FRAMEBUFFER, attachmentPoint, target, m_DepthAttachment, 0);
            }
            else {
                glCreateRenderbuffers(1, &m_DepthAttachment);
                if (m_Spec.Samples > 1) {
                    glNamedRenderbufferStorageMultisample(m_DepthAttachment, m_Spec.Samples,
                        depthSpec.InternalFormat, m_Spec.Width, m_Spec.Height);
                }
                else {
                    glNamedRenderbufferStorage(m_DepthAttachment, depthSpec.InternalFormat,
                        m_Spec.Width, m_Spec.Height);
                }
                glFramebufferRenderbuffer(GL_FRAMEBUFFER, attachmentPoint, GL_RENDERBUFFER, m_DepthAttachment);
            }
        }

        if (!drawBuffers.empty()) {
            glDrawBuffers(static_cast<GLsizei>(drawBuffers.size()), drawBuffers.data());
        }
        else {
            glDrawBuffer(GL_NONE); // Depth-only or no attachments
        }

        GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
        if (status != GL_FRAMEBUFFER_COMPLETE) {
            LH_CORE_ERROR("Framebuffer is incomplete: {0}", status);
        }

        glBindFramebuffer(GL_FRAMEBUFFER, 0);
    }

    void Framebuffer::DeleteAttachments()
    {
        // Delete color attachments
        for (size_t i = 0; i < m_ColorAttachments.size(); ++i) {
            const auto& id = m_ColorAttachments[i];
            if (m_Spec.ColorAttachments[i].IsTexture) {
                glDeleteTextures(1, &id);
            }
            else {
                glDeleteRenderbuffers(1, &id);
            }
        }
        m_ColorAttachments.clear();

        // Delete depth/stencil attachment
        if (m_DepthAttachment != 0) {
            if (m_Spec.DepthStencilAttachment.has_value()) {
                const auto& depthSpec = m_Spec.DepthStencilAttachment.value();
                if (depthSpec.IsTexture) {
                    glDeleteTextures(1, &m_DepthAttachment);
                }
                else {
                    glDeleteRenderbuffers(1, &m_DepthAttachment);
                }
            }
            m_DepthAttachment = 0;
        }
    }

    GLenum Framebuffer::GetAttachmentPoint(GLenum internalFormat) const
    {
        switch (internalFormat) {
            case GL_DEPTH_COMPONENT:
            case GL_DEPTH_COMPONENT16:
            case GL_DEPTH_COMPONENT24:
            case GL_DEPTH_COMPONENT32F:
                return GL_DEPTH_ATTACHMENT;
            case GL_STENCIL_INDEX:
            case GL_STENCIL_INDEX1:
            case GL_STENCIL_INDEX4:
            case GL_STENCIL_INDEX8:
            case GL_STENCIL_INDEX16:
                return GL_STENCIL_ATTACHMENT;
            case GL_DEPTH24_STENCIL8:
            case GL_DEPTH32F_STENCIL8:
                return GL_DEPTH_STENCIL_ATTACHMENT;
            default:
                LH_CORE_ERROR("Unsupported internal format for attachment");
                return GL_NONE;
        }
    }
}
