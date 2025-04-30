#pragma once

#include <cstdint>
#include <memory>
#include <vector>
#include <unordered_map>
#include <string>
#include <glad/glad.h>

namespace Luth
{
    class Framebuffer
    {
    public:
        struct AttachmentSpec {
            GLenum InternalFormat = GL_RGBA16F;
            GLenum Format = GL_RGBA;
            GLenum Type = GL_FLOAT;
            GLenum MinFilter = GL_LINEAR;
            GLenum MagFilter = GL_LINEAR;
            GLenum WrapS = GL_CLAMP_TO_EDGE;
            GLenum WrapT = GL_CLAMP_TO_EDGE;
            bool IsTexture = true;
            std::string Name;
        };

        struct Spec {
            u32 Width = 1280;
            u32 Height = 720;
            std::vector<AttachmentSpec> ColorAttachments;
            std::optional<AttachmentSpec> DepthStencilAttachment;
            u32 Samples = 1;
        };

        static std::shared_ptr<Framebuffer> Create(const Spec& spec);

        Framebuffer(const Spec& spec);
        ~Framebuffer();

        void Bind();
        static void Unbind();
        void Resize(u32 width, u32 height);
        void BindColorAsTexture(u32 index, u32 slot) const;
        void BindDepthAsTexture(u32 slot) const;

        // Getters
        u32 GetRendererID() const { return m_RendererID; }
        std::vector<std::pair<std::string, u32>> GetAllAttachments() const;
        u32 GetColorAttachmentID(u32 index = 0) const;
        u32 GetDepthAttachmentID() const;
        const Spec& GetSpecification() const { return m_Spec; }

    private:
        void Invalidate();
        void DeleteAttachments();
        GLenum GetAttachmentPoint(GLenum internalFormat) const;

        Spec m_Spec;
        u32 m_RendererID = 0;
        std::vector<u32> m_ColorAttachments;
        u32 m_DepthAttachment = 0;
    };
}
