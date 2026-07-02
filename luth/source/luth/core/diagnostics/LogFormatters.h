// fmt formatter specializations for STL + GLM + Vulkan types so spdlog can format them
// directly. Include before any logging statement that touches these types. Requires C++20.

#pragma once

#include "luth/core/types/LuthMath.h"

#include <spdlog/fmt/bundled/format.h>
#include <spdlog/fmt/ostr.h>
#include <vulkan/vulkan.h>
#include <filesystem>
#include <optional>
#include <variant>
#include <chrono>
#include <memory>
#include <vector>
#include <map>
#include <unordered_map>
#include <set>
#include <utility>
#include <ostream>


// ---- Standard Library Types ----
namespace fmt
{
    template <typename T>
    struct formatter<std::unique_ptr<T>> {
        constexpr auto parse(format_parse_context& ctx) { return ctx.begin(); }

        template <typename Context>
        auto format(const std::unique_ptr<T>& ptr, Context& ctx) const {
            return ptr ? format_to(ctx.out(), "unique_ptr({})", *ptr)
                : format_to(ctx.out(), "unique_ptr(nullptr)");
        }
    };

    template <typename T>
    struct formatter<std::shared_ptr<T>> {
        constexpr auto parse(format_parse_context& ctx) { return ctx.begin(); }

        template <typename Context>
        auto format(const std::shared_ptr<T>& ptr, Context& ctx) const {
            return ptr ? format_to(ctx.out(), "shared_ptr({})", *ptr)
                : format_to(ctx.out(), "shared_ptr(nullptr)");
        }
    };

    template <typename T>
    struct formatter<std::vector<T>> {
        constexpr auto parse(format_parse_context& ctx) { return ctx.begin(); }

        template <typename Context>
        auto format(const std::vector<T>& vec, Context& ctx) const {
            auto out = ctx.out();
            *out++ = '[';
            bool first = true;
            for (const auto& item : vec) {
                if (!first) {
                    *out++ = ',';
                    *out++ = ' ';
                }
                first = false;
                format_to(out, "{}", item);
            }
            *out++ = ']';
            return out;
        }
    };

    template <typename T1, typename T2>
    struct formatter<std::pair<T1, T2>> {
        constexpr auto parse(format_parse_context& ctx) { return ctx.begin(); }

        template <typename Context>
        auto format(const std::pair<T1, T2>& p, Context& ctx) const {
            return format_to(ctx.out(), "({}, {})", p.first, p.second);
        }
    };

    template <>
    struct formatter<std::filesystem::path> {
        constexpr auto parse(format_parse_context& ctx) { return ctx.begin(); }

        template <typename Context>
        auto format(const std::filesystem::path& p, Context& ctx) const {
            return format_to(ctx.out(), "{}", p.generic_string());
        }
    };

    // Formats as ISO 8601 UTC.
    template <>
    struct formatter<std::chrono::system_clock::time_point> {
        constexpr auto parse(format_parse_context& ctx) { return ctx.begin(); }

        template <typename Context>
        auto format(const std::chrono::system_clock::time_point& tp, Context& ctx) const {
            std::time_t time = std::chrono::system_clock::to_time_t(tp);
            std::tm tm = *std::gmtime(&time);  // UTC time
            return format_to(ctx.out(), "{:%Y-%m-%dT%H:%M:%SZ}", tm);
        }
    };

    template <typename T>
    struct formatter<std::optional<T>> {
        constexpr auto parse(format_parse_context& ctx) { return ctx.begin(); }

        template <typename Context>
        auto format(const std::optional<T>& opt, Context& ctx) const {
            return opt.has_value() ? format_to(ctx.out(), "{}", *opt)
                : format_to(ctx.out(), "nullopt");
        }
    };
}

// ---- Third-party Types ----
namespace fmt
{
    template <>
    struct formatter<VkResult> : formatter<string_view> {
        constexpr auto parse(format_parse_context& ctx) {
            return formatter<string_view>::parse(ctx);
        }

        template <typename FormatContext>
        auto format(const VkResult& result, FormatContext& ctx) const {
            const char* name = "Unknown VkResult";
            switch (result) {
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
                default:
                    return format_to(ctx.out(), "VkResult({})", static_cast<int>(result));
            }
            return formatter<string_view>::format(name, ctx);
        }
    };
}

// ---- Luth Types ----
namespace Luth
{
    // ostream<< adapters used by spdlog/fmt's ostream formatter
    inline std::ostream& operator<<(std::ostream& os, const Vec3& v) {
        return os << "(" << v.x << ", " << v.y << ", " << v.z << ")";
    }

    inline std::ostream& operator<<(std::ostream& os, const Mat4& m) {
        for (int i = 0; i < 4; ++i) {
            os << "\n| ";
            for (int j = 0; j < 4; ++j)
                os << m[i][j] << " ";
        }
        return os << " |";
    }
}
