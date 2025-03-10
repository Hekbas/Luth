#include "luthpch.h"
#include "luth/renderer/vulkan/VKBuffer.h"
#include "luth/renderer/vulkan/VKCommon.h"

namespace Luth
{
    // Vertex Buffer
    // ------------------------
    VKVertexBuffer::VKVertexBuffer(VkDevice device, VkPhysicalDevice physicalDevice, 
        const void* data, uint32_t size, VkQueue transferQueue, uint32_t transferQueueFamily)
        : m_Device(device), m_PhysicalDevice(physicalDevice), m_Size(size)
    {
        // Create staging resources
        VkBuffer stagingBuffer;
        VkDeviceMemory stagingMemory;
        VKUtils::CreateBuffer(
            size, 
            VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
            device, physicalDevice,
            stagingBuffer, stagingMemory
        );

        // Map and copy data
        void* mappedData;
        vkMapMemory(m_Device, stagingMemory, 0, size, 0, &mappedData);
        memcpy(mappedData, data, size);
        vkUnmapMemory(m_Device, stagingMemory);

        // Create device-local buffer
        VKUtils::CreateBuffer(
            size,
            VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
            VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
            device, physicalDevice,
            m_Buffer, m_Memory
        );

        // Copy staging -> device buffer
        VKUtils::CopyBuffer(stagingBuffer, m_Buffer, device, size, transferQueue, transferQueueFamily);

        // Cleanup staging resources
        vkDestroyBuffer(m_Device, stagingBuffer, nullptr);
        vkFreeMemory(m_Device, stagingMemory, nullptr);
    }

    VKVertexBuffer::~VKVertexBuffer() {
        vkDestroyBuffer(m_Device, m_Buffer, nullptr);
        vkFreeMemory(m_Device, m_Memory, nullptr);
    }

    void VKVertexBuffer::Bind() const {
        // Vulkan binding is handled in command buffers
    }

    void VKVertexBuffer::Unbind() const {
        // Not applicable in Vulkan
    }

    void VKVertexBuffer::SetData(const void* data, uint32_t size)
    {
        // For dynamic data, recreate with new size
        // Note: For frequent updates, consider using host-visible memory
        assert(size <= m_Size && "New data exceeds buffer capacity");

        // Create temporary staging buffer
        VkBuffer stagingBuffer;
        VkDeviceMemory stagingMemory;
        VKUtils::CreateBuffer(
            size,
            VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
            m_Device, m_PhysicalDevice,
            stagingBuffer, stagingMemory
        );

        void* mappedData;
        vkMapMemory(m_Device, stagingMemory, 0, size, 0, &mappedData);
        memcpy(mappedData, data, size);
        vkUnmapMemory(m_Device, stagingMemory);

        VKUtils::CopyBuffer(stagingBuffer, m_Buffer, m_Device, size, m_TransferQueue, m_TransferQueueFamily);

        vkDestroyBuffer(m_Device, stagingBuffer, nullptr);
        vkFreeMemory(m_Device, stagingMemory, nullptr);
    }
    

    // Index Buffer
    // ------------------------
    VKIndexBuffer::VKIndexBuffer(VkDevice device, VkPhysicalDevice physicalDevice,
        const uint32_t* indices, uint32_t count, VkQueue transferQueue, uint32_t transferQueueFamily)
        : m_Device(device), m_Count(count)
    {
        VkDeviceSize bufferSize = sizeof(uint32_t) * count;

        // Create staging buffer
        VkBuffer stagingBuffer;
        VkDeviceMemory stagingMemory;
        VKUtils::CreateBuffer(
            bufferSize,
            VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
            device, physicalDevice,
            stagingBuffer, stagingMemory
        );

        // Copy data to staging buffer
        void* data;
        vkMapMemory(m_Device, stagingMemory, 0, bufferSize, 0, &data);
        memcpy(data, indices, bufferSize);
        vkUnmapMemory(m_Device, stagingMemory);

        // Create device-local index buffer
        VKUtils::CreateBuffer(
            bufferSize,
            VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
            VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
            device, physicalDevice,
            m_Buffer, m_Memory
        );

        // Copy data to device buffer
        VKUtils::CopyBuffer(stagingBuffer, m_Buffer, device, bufferSize, transferQueue, transferQueueFamily);

        // Cleanup staging
        vkDestroyBuffer(m_Device, stagingBuffer, nullptr);
        vkFreeMemory(m_Device, stagingMemory, nullptr);
    }

    VKIndexBuffer::~VKIndexBuffer()
    {
        vkDestroyBuffer(m_Device, m_Buffer, nullptr);
        vkFreeMemory(m_Device, m_Memory, nullptr);
    }

    void VKIndexBuffer::Bind() const {
        // Binding handled during command buffer recording
    }

    void VKIndexBuffer::Unbind() const {
        // Not applicable in Vulkan
    }
}
