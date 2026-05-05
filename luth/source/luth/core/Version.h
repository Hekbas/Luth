#pragma once

#include <cstdint>

namespace Luth
{
    // ---- Single source of truth: edit ONLY these three values ----
    inline constexpr uint32_t VERSION_MAJOR = 2;
    inline constexpr uint32_t VERSION_MINOR = 9;
    inline constexpr uint32_t VERSION_PATCH = 11;

    // Optional: pre-release suffix (empty string for releases)
    inline constexpr const char* VERSION_SUFFIX = "";  // e.g., "-dev", "-rc1"

    // ---- Derived constants (do not edit) ----
    inline constexpr uint32_t VERSION_PACKED =
        (VERSION_MAJOR << 22) | (VERSION_MINOR << 12) | VERSION_PATCH;

    const char* GetVersionString();     // "1.0.0"
    const char* GetFullTitleString();   // "Luth 1.0.0 [Vulkan]"
}
