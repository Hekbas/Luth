#pragma once

#include "luth/renderer/resources/Buffer.h"
#include "luth/renderer/material/Material.h"

#include <memory>

namespace Luth
{
    class Mesh
    {
    public:
        Mesh(const std::shared_ptr<VertexBuffer>& vb, const std::shared_ptr<IndexBuffer>& ib)
            : m_VertexBuffer(vb), m_IndexBuffer(ib) {}
        ~Mesh() = default;

        const std::shared_ptr<VertexBuffer>& GetVertexBuffer() const { return m_VertexBuffer; }
        const std::shared_ptr<IndexBuffer>& GetIndexBuffer() const { return m_IndexBuffer; }

        static std::shared_ptr<Mesh> Create(
            const std::shared_ptr<VertexBuffer>& vb,
            const std::shared_ptr<IndexBuffer>& ib = nullptr);
            
    private:
        std::shared_ptr<VertexBuffer> m_VertexBuffer;
        std::shared_ptr<IndexBuffer> m_IndexBuffer;
    };
}
