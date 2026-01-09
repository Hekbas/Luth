#include "luthpch.h"
#include "VulkanBuffer.h"
#include "VulkanContext.h"

// We need VMA enums here
#include <vma/vk_mem_alloc.h>

namespace Luth
{
    // ========================================================================
    // Vertex Buffer
    // ========================================================================

    VKVertexBuffer::VKVertexBuffer(uint32_t size)
    {
        VkBufferCreateInfo bufferInfo = {};
        bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        bufferInfo.size = size;
        bufferInfo.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

        m_Allocation = VulkanAllocator::AllocateBuffer(bufferInfo, VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE, m_Buffer);
    }

    VKVertexBuffer::VKVertexBuffer(const void* data, uint32_t size)
        : VKVertexBuffer(size)
    {
        SetData(data, size);
    }

    VKVertexBuffer::~VKVertexBuffer()
    {
        VulkanContext::Get().PushDeletion([b = m_Buffer, a = m_Allocation]() {
            VulkanAllocator::FreeBuffer(b, a);
        });
    }

    void VKVertexBuffer::Bind() const
    {
        VkCommandBuffer cmd = VK_NULL_HANDLE; // TODO: Retrieve from Renderer Context
        // vkCmdBindVertexBuffers(cmd, 0, 1, &m_Buffer, offsets);
        // Binding is handled by the Pipeline/Renderer, not the buffer itself in Vulkan
    }

    void VKVertexBuffer::SetData(const void* data, uint32_t size)
    {
        // 1. Create Staging Buffer
        VkBufferCreateInfo stagingInfo = {};
        stagingInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        stagingInfo.size = size;
        stagingInfo.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
        
        VkBuffer stagingBuffer;
        VmaAllocation stagingAlloc = VulkanAllocator::AllocateBuffer(stagingInfo, VMA_MEMORY_USAGE_CPU_ONLY, stagingBuffer);

        // 2. Copy data to staging
        void* mappedData = VulkanAllocator::Map(stagingAlloc);
        memcpy(mappedData, data, size);
        VulkanAllocator::Unmap(stagingAlloc);

        // 3. Copy from staging to GPU buffer
        VulkanContext::Get().ImmediateSubmit([=](VkCommandBuffer cmd) {
            VkBufferCopy copyRegion{};
            copyRegion.srcOffset = 0;
            copyRegion.dstOffset = 0;
            copyRegion.size = size;
            vkCmdCopyBuffer(cmd, stagingBuffer, m_Buffer, 1, &copyRegion);
        });

        // 4. Cleanup
        VulkanAllocator::FreeBuffer(stagingBuffer, stagingAlloc);
    }

    // ========================================================================
    // Index Buffer
    // ========================================================================

    VKIndexBuffer::VKIndexBuffer(const uint32_t* indices, uint32_t count)
        : m_Count(count)
    {
        VkDeviceSize size = sizeof(uint32_t) * count;

        VkBufferCreateInfo bufferInfo = {};
        bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        bufferInfo.size = size;
        bufferInfo.usage = VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

        m_Allocation = VulkanAllocator::AllocateBuffer(bufferInfo, VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE, m_Buffer);

        // Upload Data
        VkBufferCreateInfo stagingInfo = {};
        stagingInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        stagingInfo.size = size;
        stagingInfo.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;

        VkBuffer stagingBuffer;
        VmaAllocation stagingAlloc = VulkanAllocator::AllocateBuffer(stagingInfo, VMA_MEMORY_USAGE_CPU_ONLY, stagingBuffer);

        void* mappedData = VulkanAllocator::Map(stagingAlloc);
        memcpy(mappedData, indices, size);
        VulkanAllocator::Unmap(stagingAlloc);

        VulkanContext::Get().ImmediateSubmit([=](VkCommandBuffer cmd) {
            VkBufferCopy copyRegion{};
            copyRegion.size = size;
            vkCmdCopyBuffer(cmd, stagingBuffer, m_Buffer, 1, &copyRegion);
        });

        VulkanAllocator::FreeBuffer(stagingBuffer, stagingAlloc);
    }

    VKIndexBuffer::~VKIndexBuffer()
    {
        VulkanContext::Get().PushDeletion([b = m_Buffer, a = m_Allocation]() {
            VulkanAllocator::FreeBuffer(b, a);
        });
    }

    void VKIndexBuffer::Bind() const {}

    // ========================================================================
    // Uniform Buffer
    // ========================================================================

    VKUniformBuffer::VKUniformBuffer(uint32_t size)
    {
        VkBufferCreateInfo bufferInfo = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
        bufferInfo.size = size;
        bufferInfo.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
        bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

        // CPU_TO_GPU is persistently mapped
        m_Allocation = VulkanAllocator::AllocateBuffer(bufferInfo, VMA_MEMORY_USAGE_CPU_TO_GPU, m_Buffer);
        m_MappedData = VulkanAllocator::Map(m_Allocation);
    }

    VKUniformBuffer::~VKUniformBuffer()
    {
        VulkanAllocator::Unmap(m_Allocation);
        VulkanContext::Get().PushDeletion([b = m_Buffer, a = m_Allocation]() {
            VulkanAllocator::FreeBuffer(b, a);
        });
    }

    void VKUniformBuffer::SetData(const void* data, uint32_t size, uint32_t offset)
    {
        memcpy((uint8_t*)m_MappedData + offset, data, size);
    }
}