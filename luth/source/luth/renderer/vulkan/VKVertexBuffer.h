#pragma once

#include "luth/renderer/VertexBuffer.h"

#include <vulkan/vulkan.h>

namespace Luth
{
    class VKVertexBuffer : public VertexBuffer
    {
    public:
        VKVertexBuffer(VkDevice device, VkPhysicalDevice physicalDevice, uint32_t size);
        VKVertexBuffer(VkDevice device, VkPhysicalDevice physicalDevice,
            const void* data, uint32_t size);
        ~VKVertexBuffer();

        void Bind() const override;
        void Unbind() const override;
        void SetData(const void* data, uint32_t size) override;

        VkBuffer GetHandle() const { return m_Buffer; }

    private:
        void CreateBuffer(uint32_t size, VkBufferUsageFlags usage,
            VkMemoryPropertyFlags properties,
            VkBuffer& buffer, VkDeviceMemory& memory);

        VkDevice m_Device;
        VkBuffer m_Buffer;
        VkDeviceMemory m_Memory;
        void* m_MappedData = nullptr;
        uint32_t m_Size;
    };
}
