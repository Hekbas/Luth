#pragma once

#include <cstdint>

namespace Luth
{
    // ---- Source of truth: edit ONLY these three values ----
    inline constexpr uint32_t VERSION_MAJOR = 3;
    inline constexpr uint32_t VERSION_MINOR = 8;
    inline constexpr uint32_t VERSION_PATCH = 0;

    // Empty for releases; otherwise "-dev", "-rc1", etc.
    inline constexpr const char* VERSION_SUFFIX = "";

    // ---- Derived ----
    inline constexpr uint32_t VERSION_PACKED =
        (VERSION_MAJOR << 22) | (VERSION_MINOR << 12) | VERSION_PATCH;

    const char* GetVersionString();     // "1.0.0"
    const char* GetFullTitleString();   // "Luth 1.0.0 [Vulkan]"
}
