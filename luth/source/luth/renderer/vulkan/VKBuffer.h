#pragma once

#include "luth/renderer/Buffer.h"

#include <vulkan/vulkan.h>

namespace Luth
{
    class VKVertexBuffer : public VertexBuffer
    {
    public:
        VKVertexBuffer(VkDevice device, VkPhysicalDevice physicalDevice, uint32_t size);
        VKVertexBuffer(VkDevice device, VkPhysicalDevice physicalDevice,
            const void* data, uint32_t size, VkQueue transferQueue, uint32_t transferQueueFamily);
        ~VKVertexBuffer();

        void Bind() const override;
        void Unbind() const override;
        void SetData(const void* data, uint32_t size) override;

        const BufferLayout& GetLayout() const override { return m_Layout; }
        void SetLayout(const BufferLayout& layout) override { m_Layout = layout; }

        VkBuffer GetBuffer() const { return m_Buffer; }
        uint32_t GetVertexCount() const { return m_Size / m_Layout.GetStride(); }

    private:
        VkDevice m_Device;
        VkPhysicalDevice m_PhysicalDevice;
        VkBuffer m_Buffer;
        VkDeviceMemory m_Memory;
        void* m_MappedData = nullptr;
        uint32_t m_Size;
        BufferLayout m_Layout;
        VkQueue m_TransferQueue;
        uint32_t m_TransferQueueFamily;
    };

    class VKIndexBuffer : public IndexBuffer
    {
    public:
        VKIndexBuffer(VkDevice device, VkPhysicalDevice physicalDevice,
            const uint32_t* indices, uint32_t count, VkQueue transferQueue, uint32_t transferQueueFamily);
        ~VKIndexBuffer();

        void Bind() const override;
        void Unbind() const override;
        uint32_t GetCount() const override { return m_Count; }

        VkBuffer GetBuffer() const { return m_Buffer; }

    private:
        VkDevice m_Device;
        VkBuffer m_Buffer;
        VkDeviceMemory m_Memory;
        uint32_t m_Count;
    };
}
