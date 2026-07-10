#include "luthpch.h"
#include "VulkanBuffer.h"
#include "VulkanContext.h"
#include "VulkanAllocator.h"
#include "UploadContext.h"

// VMA enums used here
#include <vma/vk_mem_alloc.h>

namespace Luth
{
    // ---- Vertex Buffer ----

    VKVertexBuffer::VKVertexBuffer(uint32_t size)
    {
        LH_PROFILE_FUNCTION();

        VkBufferCreateInfo bufferInfo = {};
        bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        bufferInfo.size = size;
        // AS_BUILD_INPUT_READ_ONLY required by vkCmdBuildAccelerationStructuresKHR per VUID-...-03671.
        bufferInfo.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT
                         | VK_BUFFER_USAGE_TRANSFER_DST_BIT
                         | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT
                         | VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR;
        bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        // The deferred static BLAS build reads the VB via BDA on the AsyncCompute queue; EXCLUSIVE +
        // cross-queue access without QFOT is spec-undefined and TDRs on NVIDIA (mirrors the IB below).
        // Single-family GPUs silently fall back to EXCLUSIVE. see arch/multi-queue.md
        VulkanContext::Get().ApplyConcurrentSharing(bufferInfo);

        m_Allocation = VulkanAllocator::AllocateBuffer(bufferInfo, VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE, m_Buffer);

        VkBufferDeviceAddressInfo addrInfo{ VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO };
        addrInfo.buffer = m_Buffer;
        m_DeviceAddress = vkGetBufferDeviceAddress(VulkanContext::Get().GetDevice(), &addrInfo);
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
        // Binding is handled by the Pipeline/Renderer, not the buffer itself in Vulkan
    }

    void VKVertexBuffer::SetData(const void* data, uint32_t size)
    {
        // Async transfer through UploadContext's staging ring + transfer-queue submit; the caller never blocks
        // on the fence. m_UploadFence is the timeline value consumers gate on (UploadContext::IsComplete): the
        // draw list + skinning dispatch + the deferred BLAS build each skip this mesh until it retires, so
        // nothing reads the buffer cross-queue before the DMA completes. see arch/multi-queue.md
        m_UploadFence = UploadContext::Get().UploadBuffer(data, size, m_Buffer, 0);
    }

    // ---- Index Buffer ----

    VKIndexBuffer::VKIndexBuffer(const uint32_t* indices, uint32_t count)
        : m_Count(count)
    {
        LH_PROFILE_FUNCTION();

        VkDeviceSize size = sizeof(uint32_t) * count;

        VkBufferCreateInfo bufferInfo = {};
        bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        bufferInfo.size = size;
        // AS_BUILD_INPUT_READ_ONLY required by vkCmdBuildAccelerationStructuresKHR per VUID-...-03672.
        bufferInfo.usage = VK_BUFFER_USAGE_INDEX_BUFFER_BIT
                         | VK_BUFFER_USAGE_TRANSFER_DST_BIT
                         | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT
                         | VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR;
        bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        // Skinned BLAS refit reads the IB via BDA on the AsyncCompute queue (see TlasBuilder::
        // RefitSkinnedBLASes). EXCLUSIVE + cross-queue access without QFOT is spec-undefined and
        // TDRs on NVIDIA. Per arch/multi-queue.md, opt into CONCURRENT for the deduped family
        // set; single-family GPUs silently fall back to EXCLUSIVE.
        VulkanContext::Get().ApplyConcurrentSharing(bufferInfo);

        m_Allocation = VulkanAllocator::AllocateBuffer(bufferInfo, VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE, m_Buffer);

        VkBufferDeviceAddressInfo addrInfo{ VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO };
        addrInfo.buffer = m_Buffer;
        m_DeviceAddress = vkGetBufferDeviceAddress(VulkanContext::Get().GetDevice(), &addrInfo);

        // Async upload; see VKVertexBuffer::SetData for the queue-ordering rationale.
        m_UploadFence = UploadContext::Get().UploadBuffer(indices, size, m_Buffer, 0);
    }

    VKIndexBuffer::~VKIndexBuffer()
    {
        VulkanContext::Get().PushDeletion([b = m_Buffer, a = m_Allocation]() {
            VulkanAllocator::FreeBuffer(b, a);
        });
    }

    void VKIndexBuffer::Bind() const {}

    // ---- Uniform Buffer ----

    VKUniformBuffer::VKUniformBuffer(uint32_t size)
    {
        LH_PROFILE_FUNCTION();

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
