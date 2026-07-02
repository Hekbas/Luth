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

    // ---- Primitive types ----
    using i8  = int8_t;
    using i16 = int16_t;
    using i32 = int32_t;
    using i64 = int64_t;

    using u8  = uint8_t;
    using u16 = uint16_t;
    using u32 = uint32_t;
    using u64 = uint64_t;

    using f32 = float;
    using f64 = double;

    using byte = std::byte;

    // ---- Static assertions (safety) ----
    static_assert(sizeof(Luth::i32) == 4, "i32 must be 4 bytes!");
    static_assert(sizeof(Luth::f32) == 4, "f32 must be 4 bytes!");
}
