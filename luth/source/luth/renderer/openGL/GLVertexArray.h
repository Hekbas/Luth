#pragma once

#include "luth/renderer/VertexArray.h"

#include <vector>

namespace Luth
{
    class GLVertexArray : public VertexArray
    {
    public:
        GLVertexArray();
        ~GLVertexArray();

        void Bind() const override;
        void Unbind() const override;

        void AddVertexBuffer(const std::shared_ptr<VertexBuffer>& vb) override;
        void SetIndexBuffer(const std::shared_ptr<IndexBuffer>& ib) override;

        const std::vector<std::shared_ptr<VertexBuffer>>& GetVertexBuffers() const override { return m_VertexBuffers; }
        const std::shared_ptr<IndexBuffer>& GetIndexBuffer() const override { return m_IndexBuffer; }

    private:
        uint32_t m_VertexArrayID;
        std::vector<std::shared_ptr<VertexBuffer>> m_VertexBuffers;
        std::shared_ptr<IndexBuffer> m_IndexBuffer;
    };
}
