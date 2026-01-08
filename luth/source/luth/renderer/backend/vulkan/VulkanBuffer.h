#pragma once

#include "luth/renderer/Buffer.h"
#include "VulkanAllocator.h"

namespace Luth
{
    class VKVertexBuffer : public VertexBuffer
    {
    public:
        VKVertexBuffer(uint32_t size);
        VKVertexBuffer(const void* data, uint32_t size);
        virtual ~VKVertexBuffer();

        virtual void Bind() const override;
        virtual void Unbind() const override {}

        virtual void SetData(const void* data, uint32_t size) override;

        virtual const BufferLayout& GetLayout() const override { return m_Layout; }
        virtual void SetLayout(const BufferLayout& layout) override { m_Layout = layout; }

        VkBuffer GetVulkanBuffer() const { return m_Buffer; }

    private:
        VkBuffer m_Buffer = VK_NULL_HANDLE;
        VmaAllocation m_Allocation = nullptr;
        BufferLayout m_Layout;
    };

    class VKIndexBuffer : public IndexBuffer
    {
    public:
        VKIndexBuffer(const uint32_t* indices, uint32_t count);
        virtual ~VKIndexBuffer();

        virtual void Bind() const override;
        virtual void Unbind() const override {}

        virtual uint32_t GetCount() const override { return m_Count; }
        
        VkBuffer GetVulkanBuffer() const { return m_Buffer; }

    private:
        VkBuffer m_Buffer = VK_NULL_HANDLE;
        VmaAllocation m_Allocation = nullptr;
        uint32_t m_Count;
    };
}