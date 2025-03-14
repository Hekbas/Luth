#pragma once

#include "luth/core/Log.h"
#include "luth/utils/CustomFormatters.h"

#define VK_CHECK_RESULT(result, message)												\
	do {																				\
		if (result != VK_SUCCESS) {														\
			LH_CORE_ASSERT(false, "Vulkan Error: {0} (Code: {1})", message, result);	\
		}																				\
	} while (0);

namespace Luth::VKUtils
{
	uint32_t FindMemoryType(VkPhysicalDevice physicalDevice,
		uint32_t typeFilter,
		VkMemoryPropertyFlags properties);

	void CreateBuffer(uint32_t size, VkBufferUsageFlags usage, VkMemoryPropertyFlags properties,
		VkDevice device, VkPhysicalDevice physicalDevice, VkBuffer& buffer, VkDeviceMemory& memory);

	void CopyBuffer(VkBuffer src, VkBuffer dst, VkDevice device, VkDeviceSize size,
		VkQueue queue, uint32_t queueFamily);
}
