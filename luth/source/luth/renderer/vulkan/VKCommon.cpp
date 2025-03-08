#include "luthpch.h"
#include "luth/renderer/vulkan/VKCommon.h"

namespace Luth::VKUtils
{
    uint32_t FindMemoryType(VkPhysicalDevice physicalDevice,
        uint32_t typeFilter,
        VkMemoryPropertyFlags properties)
    {
        VkPhysicalDeviceMemoryProperties memProperties;
        vkGetPhysicalDeviceMemoryProperties(physicalDevice, &memProperties);

        for (uint32_t i = 0; i < memProperties.memoryTypeCount; i++) {
            if ((typeFilter & (1 << i)) &&
                (memProperties.memoryTypes[i].propertyFlags & properties) == properties) {
                return i;
            }
        }

        LH_CORE_ASSERT(false, "Failed to find suitable memory type!");
        return 0;
    }
}
