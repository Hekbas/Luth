#pragma once

#include "luth/renderer/VertexBuffer.h"

namespace Luth
{
    class GLVertexBuffer : public VertexBuffer
    {
    public:
        GLVertexBuffer(uint32_t size);
        GLVertexBuffer(const void* data, uint32_t size);
        ~GLVertexBuffer();

        void Bind() const override;
        void Unbind() const override;
        void SetData(const void* data, uint32_t size) override;

        void SetLayout(const BufferLayout& layout) { m_Layout = layout; }
        const BufferLayout& GetLayout() const { return m_Layout; }

    private:
        uint32_t m_RendererID;
        BufferLayout m_Layout;
    };
}
