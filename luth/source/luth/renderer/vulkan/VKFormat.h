#pragma once

#include <vulkan/vulkan.h>
#include <spdlog/fmt/bundled/format.h>

template <>
struct fmt::formatter<VkResult> : fmt::formatter<fmt::string_view>
{
    template <typename FormatContext>
    constexpr auto parse(FormatContext& ctx) {
        return ctx.begin();
    }

    template <typename FormatContext>
    auto format(const VkResult& result, FormatContext& ctx) const {
        const char* name = "Unknown VkResult";
        switch (result)
        {
            case VK_SUCCESS:                        name = "VK_SUCCESS";                        break;
            case VK_NOT_READY:                      name = "VK_NOT_READY";                      break;
            case VK_TIMEOUT:                        name = "VK_TIMEOUT";                        break;
            case VK_EVENT_SET:                      name = "VK_EVENT_SET";                      break;
            case VK_EVENT_RESET:                    name = "VK_EVENT_RESET";                    break;
            case VK_INCOMPLETE:                     name = "VK_INCOMPLETE";                     break;
            case VK_ERROR_OUT_OF_HOST_MEMORY:       name = "VK_ERROR_OUT_OF_HOST_MEMORY";       break;
            case VK_ERROR_OUT_OF_DEVICE_MEMORY:     name = "VK_ERROR_OUT_OF_DEVICE_MEMORY";     break;
            case VK_ERROR_INITIALIZATION_FAILED:    name = "VK_ERROR_INITIALIZATION_FAILED";    break;
            case VK_ERROR_DEVICE_LOST:              name = "VK_ERROR_DEVICE_LOST";              break;
            case VK_ERROR_MEMORY_MAP_FAILED:        name = "VK_ERROR_MEMORY_MAP_FAILED";        break;
            case VK_ERROR_LAYER_NOT_PRESENT:        name = "VK_ERROR_LAYER_NOT_PRESENT";        break;
            case VK_ERROR_EXTENSION_NOT_PRESENT:    name = "VK_ERROR_EXTENSION_NOT_PRESENT";    break;
            case VK_ERROR_FEATURE_NOT_PRESENT:      name = "VK_ERROR_FEATURE_NOT_PRESENT";      break;
            case VK_ERROR_INCOMPATIBLE_DRIVER:      name = "VK_ERROR_INCOMPATIBLE_DRIVER";      break;
            case VK_ERROR_TOO_MANY_OBJECTS:         name = "VK_ERROR_TOO_MANY_OBJECTS";         break;
            case VK_ERROR_FORMAT_NOT_SUPPORTED:     name = "VK_ERROR_FORMAT_NOT_SUPPORTED";     break;
            case VK_ERROR_SURFACE_LOST_KHR:         name = "VK_ERROR_SURFACE_LOST_KHR";         break;
            case VK_SUBOPTIMAL_KHR:                 name = "VK_SUBOPTIMAL_KHR";                 break;
            case VK_ERROR_OUT_OF_DATE_KHR:          name = "VK_ERROR_OUT_OF_DATE_KHR";          break;
            case VK_ERROR_INCOMPATIBLE_DISPLAY_KHR: name = "VK_ERROR_INCOMPATIBLE_DISPLAY_KHR"; break;
            case VK_ERROR_NATIVE_WINDOW_IN_USE_KHR: name = "VK_ERROR_NATIVE_WINDOW_IN_USE_KHR"; break;
            case VK_ERROR_VALIDATION_FAILED_EXT:    name = "VK_ERROR_VALIDATION_FAILED_EXT";    break;
            case VK_RESULT_MAX_ENUM:                name = "VK_RESULT_MAX_ENUM";                break;
            case VK_ERROR_FRAGMENTED_POOL:          name = "VK_ERROR_FRAGMENTED_POOL";          break;
            case VK_ERROR_UNKNOWN:                  name = "VK_ERROR_UNKNOWN";                  break;
        }
        return fmt::formatter<fmt::string_view>::format(name, ctx);
    }
};
