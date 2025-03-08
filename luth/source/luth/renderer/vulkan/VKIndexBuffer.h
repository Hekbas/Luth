#pragma once

#include "luth/renderer/IndexBuffer.h"

#include <vulkan/vulkan.h>

namespace Luth
{
    class VKIndexBuffer : public IndexBuffer
    {
    public:
        VKIndexBuffer(VkDevice device, VkPhysicalDevice physicalDevice, const uint32_t* indices, uint32_t count);
        ~VKIndexBuffer();

        void Bind() const override;
        void Unbind() const override;
        uint32_t GetCount() const override { return m_Count; }

        VkBuffer GetHandle() const { return m_Buffer; }

    private:
        VkDevice m_Device;
        VkBuffer m_Buffer;
        VkDeviceMemory m_Memory;
        uint32_t m_Count;
    };
}
