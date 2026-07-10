#pragma once

#include "luth/renderer/resources/Buffer.h"
#include "VulkanAllocator.h"

namespace Luth
{
    // Vulkan-backed Vertex / Index / Uniform buffer wrappers. VMA-allocated; the VkBuffer is
    // owned for the full lifetime of the wrapper. Vertex and index data is staged through
    // UploadContext (see VulkanBuffer.cpp::SetData for the queue-ordering rationale). Uniform
    // buffers are persistently mapped, so writes can land at any time without a fence.
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
        VkDeviceAddress GetDeviceAddress() const { return m_DeviceAddress; }
        // Fence value signaled by UploadContext when this buffer's most recent upload retires.
        // BLAS factory polls UploadContext::WaitForUpload(this) before reading VB on the graphics queue.
        uint64_t GetUploadFence() const override { return m_UploadFence; }

    private:
        VkBuffer m_Buffer = VK_NULL_HANDLE;
        VmaAllocation m_Allocation = nullptr;
        VkDeviceAddress m_DeviceAddress = 0;
        uint64_t m_UploadFence = 0;
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
        VkDeviceAddress GetDeviceAddress() const { return m_DeviceAddress; }
        uint64_t GetUploadFence() const override { return m_UploadFence; }

    private:
        VkBuffer m_Buffer = VK_NULL_HANDLE;
        VmaAllocation m_Allocation = nullptr;
        VkDeviceAddress m_DeviceAddress = 0;
        uint64_t m_UploadFence = 0;
        uint32_t m_Count;
    };

    class VKUniformBuffer
    {
    public:
        VKUniformBuffer(uint32_t size);
        ~VKUniformBuffer();

        void SetData(const void* data, uint32_t size, uint32_t offset = 0);
        VkBuffer GetVulkanBuffer() const { return m_Buffer; }

    private:
        VkBuffer m_Buffer = VK_NULL_HANDLE;
        VmaAllocation m_Allocation = nullptr;
        void* m_MappedData = nullptr;
    };
}