#pragma once

// Engine-wide primitive aliases (i32, f32, byte, ...) and the std::filesystem alias `fs`.
// Headers that need only primitives include this instead of pulling LuthMath.h, which also
// drags in GLM. See LuthMath.h for vector / matrix / quaternion aliases and the Math:: facade.

#include <cstdint>
#include <cstddef>
#include <filesystem>

namespace Luth
{
    namespace fs = std::filesystem;

    // ── Primitive types ──
    using i8  = int8_t;     //  8-bit signed integer
    using i16 = int16_t;    // 16-bit signed integer
    using i32 = int32_t;    // 32-bit signed integer
    using i64 = int64_t;    // 64-bit signed integer

    using u8  = uint8_t;    //  8-bit unsigned integer
    using u16 = uint16_t;   // 16-bit unsigned integer
    using u32 = uint32_t;   // 32-bit unsigned integer
    using u64 = uint64_t;   // 64-bit unsigned integer

    using f32 = float;      // 32-bit floating point
    using f64 = double;     // 64-bit floating point

    using byte = std::byte; // Standard byte type

    // ── Static assertions (safety) ──
    static_assert(sizeof(Luth::i32) == 4, "i32 must be 4 bytes!");
    static_assert(sizeof(Luth::f32) == 4, "f32 must be 4 bytes!");
}
