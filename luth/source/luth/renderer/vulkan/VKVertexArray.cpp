#include "luthpch.h"
#include "luth/renderer/vulkan/VKVertexArray.h"
#include "luth/renderer/vulkan/VKVertexBuffer.h"

namespace Luth
{
    VKVertexArray::VKVertexArray(VkDevice device) : m_Device(device) {}

    VKVertexArray::~VKVertexArray() {
        // Vulkan resources are managed by the buffers themselves
    }

    void VKVertexArray::Bind() const {
        // Binding is handled during command buffer recording
    }

    void VKVertexArray::Unbind() const {
        // Not applicable in Vulkan
    }

    void VKVertexArray::AddVertexBuffer(const std::shared_ptr<VertexBuffer>& vb) {
        auto vkBuffer = std::static_pointer_cast<VKVertexBuffer>(vb);

        // Generate binding and attribute descriptions
        // This would parse the buffer layout and populate descriptions

        m_VertexBuffers.push_back(vb);
    }

    void VKVertexArray::SetIndexBuffer(const std::shared_ptr<IndexBuffer>& ib) {
        m_IndexBuffer = ib;
    }
}
