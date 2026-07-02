#pragma once

#include "luth/core/types/LuthTypes.h"
#include <filesystem>
#include <span>

namespace Luth::MaterialLayoutGuard
{
    // One C++ struct member: name (must match the Slang field name) + its byte offset (offsetof).
    struct CppField { const char* name; size_t offset; };

    // Reflect `typeName` from `slangPath` (Slang) and assert its field offsets + total size match the C++
    // table. A real drift logs a per-field dump + LH_CORE_ERROR (asserts in debug); a reflection that can't
    // run logs a WARN and is skipped, never blocks boot. Returns true iff reflected-and-matching.
    bool Validate(const std::filesystem::path& slangPath, const char* typeName,
                  std::span<const CppField> cppFields, size_t cppSize);
}
