#pragma once

#include "luth/renderer/resources/Buffer.h"
#include "luth/renderer/material/Material.h"

#include <memory>

namespace Luth
{
    class VKAccelerationStructure;

    // Vertex and index buffer pair, owned by a Model. Each importer mesh primitive becomes one
    // Mesh. Sharing the same Mesh across MeshRenderer components avoids duplicating GPU buffers
    // for identical geometry. The optional BLAS (built at import on RT-capable backends) is
    // referenced by per-frame TLAS instance entries.
    class Mesh
    {
    public:
        Mesh(const std::shared_ptr<VertexBuffer>& vb,
             const std::shared_ptr<IndexBuffer>& ib,
             uint32_t vertexCount,
             bool isSkinned)
            : m_VertexBuffer(vb), m_IndexBuffer(ib),
              m_VertexCount(vertexCount), m_IsSkinned(isSkinned) {}
        ~Mesh();

        const std::shared_ptr<VertexBuffer>& GetVertexBuffer() const { return m_VertexBuffer; }
        const std::shared_ptr<IndexBuffer>&  GetIndexBuffer()  const { return m_IndexBuffer; }
        const std::shared_ptr<VKAccelerationStructure>& GetBlas() const { return m_Blas; }
        void SetBlas(const std::shared_ptr<VKAccelerationStructure>& blas) { m_Blas = blas; }
        uint32_t GetVertexCount() const { return m_VertexCount; }
        bool     IsSkinned()      const { return m_IsSkinned; }

        static std::shared_ptr<Mesh> Create(
            const std::shared_ptr<VertexBuffer>& vb,
            const std::shared_ptr<IndexBuffer>& ib,
            uint32_t vertexCount,
            bool isSkinned = false);

    private:
        std::shared_ptr<VertexBuffer> m_VertexBuffer;
        std::shared_ptr<IndexBuffer>  m_IndexBuffer;
        std::shared_ptr<VKAccelerationStructure> m_Blas;
        uint32_t m_VertexCount = 0;
        bool     m_IsSkinned   = false;
    };
}
