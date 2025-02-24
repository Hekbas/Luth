#pragma once

#include "luth/core/Log.h"
#include "luth/renderer/vulkan/VKFormat.h"

#define VK_CHECK_RESULT(result, message)												\
	do {																				\
		if (result != VK_SUCCESS) {														\
			LH_CORE_ASSERT(false, "Vulkan Error: {0} (Code: {1})", message, result);	\
		}																				\
	} while (0);
