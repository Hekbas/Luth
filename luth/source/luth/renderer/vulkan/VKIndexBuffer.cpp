#include "luthpch.h"
#include "luth/renderer/vulkan/VKIndexBuffer.h"
#include "luth/renderer/vulkan/VKCommon.h"

namespace Luth
{
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
