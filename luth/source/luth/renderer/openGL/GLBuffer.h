#pragma once

#include "luth/renderer/Buffer.h"

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
        uint32_t m_BufferID;
        BufferLayout m_Layout;
    };

    class GLIndexBuffer : public IndexBuffer
    {
    public:
        GLIndexBuffer(const uint32_t* indices, uint32_t count);
        ~GLIndexBuffer();

        void Bind() const override;
        void Unbind() const override;
        uint32_t GetCount() const override { return m_Count; }

    private:
        uint32_t m_BufferID;
        uint32_t m_Count;
    };
}
