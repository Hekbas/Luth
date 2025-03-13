#pragma once

#include "luth/renderer/Mesh.h"
#include "luth/renderer/vulkan/VKBuffer.h"

#include <memory>

namespace Luth
{
    class VKMesh : public Mesh
    {
    public:
        VKMesh(const std::shared_ptr<VKVertexBuffer>& vb,
            const std::shared_ptr<VKIndexBuffer>& ib = nullptr)
            : m_VertexBuffer(vb), m_IndexBuffer(ib) {}

        void Bind() const override {
            // Actual binding happens during command buffer recording
        }

        void Draw() const override {
            // Drawing logic is handled by the renderer
        }

        VkBuffer GetVertexBuffer() const { return m_VertexBuffer->GetBuffer(); }
        VkBuffer GetIndexBuffer() const { return m_IndexBuffer ? m_IndexBuffer->GetBuffer() : VK_NULL_HANDLE; }
        uint32_t GetIndexCount() const { return m_IndexBuffer ? m_IndexBuffer->GetCount() : 0; }
        uint32_t GetVertexCount() const { return m_VertexBuffer->GetVertexCount(); }

    private:
        std::shared_ptr<VKVertexBuffer> m_VertexBuffer;
        std::shared_ptr<VKIndexBuffer> m_IndexBuffer;
    };
}
