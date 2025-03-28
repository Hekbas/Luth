#pragma once

#include <cstdint>
#include <memory>

namespace Luth
{
    class Framebuffer
    {
    public:
        struct Spec {
            uint32_t Width = 1280;
            uint32_t Height = 720;
        };

        static std::shared_ptr<Framebuffer> Create(const Spec& spec);

        Framebuffer(const Spec& spec);
        ~Framebuffer();

        void Bind();
        void Unbind();
        void Resize(uint32_t width, uint32_t height);

        uint32_t GetRendererID() const { return m_RendererID; }
        uint32_t GetWidth() const { return m_Specification.Width; }
        uint32_t GetHeight() const { return m_Specification.Height; }
        uint32_t GetColorAttachmentRendererID() const { return m_ColorAttachment; }
        const Spec& GetSpecification() const { return m_Specification; }

    private:
        void Invalidate();

    private:
        uint32_t m_RendererID = 0;
        uint32_t m_ColorAttachment = 0;
        uint32_t m_DepthAttachment = 0;
        Spec m_Specification;
    };
}
