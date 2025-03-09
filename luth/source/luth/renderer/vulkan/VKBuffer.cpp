#include "luthpch.h"
#include "luth/renderer/vulkan/VKBuffer.h"
#include "luth/renderer/vulkan/VKCommon.h"

namespace Luth
{
    // Vertex Buffer
    // ------------------------
    VKVertexBuffer::VKVertexBuffer(VkDevice device, VkPhysicalDevice physicalDevice,
        const void* data, uint32_t size)
        : m_Device(device), m_Size(size)
    {
        // Create staging buffer
        VkBuffer stagingBuffer;
        VkDeviceMemory stagingMemory;
        CreateBuffer(size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
            VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
            stagingBuffer, stagingMemory);

        // Copy data to staging buffer
        vkMapMemory(m_Device, stagingMemory, 0, size, 0, &m_MappedData);
        memcpy(m_MappedData, data, size);
        vkUnmapMemory(m_Device, stagingMemory);

        // Create device-local vertex buffer
        CreateBuffer(size, VK_BUFFER_USAGE_TRANSFER_DST_BIT |
            VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
            VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
            m_Buffer, m_Memory);

        m_Layout = BufferLayout();

        // Copy staging to device buffer
        // ... (Requires command buffer - implement in VKRendererAPI)
    }

    void VKVertexBuffer::Bind() const {
        // Vulkan binding is handled in command buffers
    }

    void VKVertexBuffer::Unbind() const {
        // Not applicable in Vulkan
    }

    void VKVertexBuffer::SetData(const void* data, uint32_t size) {
        // Implement Vulkan-specific data update logic
        if (m_MappedData) {
            memcpy(m_MappedData, data, size);
        }
    }

    void VKVertexBuffer::CreateBuffer(uint32_t size, VkBufferUsageFlags usage,
        VkMemoryPropertyFlags properties,
        VkBuffer& buffer, VkDeviceMemory& memory)
    {
        // Implementation with vkCreateBuffer, vkAllocateMemory, vkBindBufferMemory
        // ... (Similar to swapchain creation logic)
    }

    // Implement other methods with proper Vulkan commands

    // Index Buffer
    // ------------------------
    VKIndexBuffer::VKIndexBuffer(VkDevice device, VkPhysicalDevice physicalDevice,
        const uint32_t* indices, uint32_t count)
        : m_Device(device), m_Count(count)
    {
        VkBufferCreateInfo bufferInfo{};
        bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        bufferInfo.size = count * sizeof(uint32_t);
        bufferInfo.usage = VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

        VK_CHECK_RESULT(vkCreateBuffer(m_Device, &bufferInfo, nullptr, &m_Buffer),
            "Failed to create index buffer!");

        // Memory allocation and binding
        VkMemoryRequirements memRequirements;
        vkGetBufferMemoryRequirements(m_Device, m_Buffer, &memRequirements);

        VkMemoryAllocateInfo allocInfo{};
        allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        allocInfo.allocationSize = memRequirements.size;
        allocInfo.memoryTypeIndex = VKUtils::FindMemoryType(
            physicalDevice,
            memRequirements.memoryTypeBits,
            VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT
        );

        VK_CHECK_RESULT(vkAllocateMemory(m_Device, &allocInfo, nullptr, &m_Memory),
            "Failed to allocate index buffer memory!");
        vkBindBufferMemory(m_Device, m_Buffer, m_Memory, 0);

        // Staging buffer for data upload
        // ... (implementation similar to vertex buffer)
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
