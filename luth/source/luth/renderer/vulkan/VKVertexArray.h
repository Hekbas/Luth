#pragma once

#include "luth/renderer/VertexArray.h"

#include <vector>
#include <vulkan/vulkan.h>

namespace Luth
{
    class VKVertexArray : public VertexArray
    {
    public:
        VKVertexArray(VkDevice device);
        ~VKVertexArray();

        void Bind() const override;
        void Unbind() const override;

        void AddVertexBuffer(const std::shared_ptr<VertexBuffer>& vb) override;
        void SetIndexBuffer(const std::shared_ptr<IndexBuffer>& ib) override;

        const std::vector<std::shared_ptr<VertexBuffer>>& GetVertexBuffers() const override { return m_VertexBuffers; }
        const std::shared_ptr<IndexBuffer>& GetIndexBuffer() const override { return m_IndexBuffer; }

        // Vulkan-specific methods
        const std::vector<VkVertexInputBindingDescription>& GetBindingDescriptions() const { return m_BindingDescriptions; }
        const std::vector<VkVertexInputAttributeDescription>& GetAttributeDescriptions() const { return m_AttributeDescriptions; }

    private:
        VkDevice m_Device;
        std::vector<std::shared_ptr<VertexBuffer>> m_VertexBuffers;
        std::shared_ptr<IndexBuffer> m_IndexBuffer;

        std::vector<VkVertexInputBindingDescription> m_BindingDescriptions;
        std::vector<VkVertexInputAttributeDescription> m_AttributeDescriptions;
    };
}
