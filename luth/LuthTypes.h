#pragma once

#include <cstdint>
#include <limits>
#include <glm/glm.hpp>
#include <type_traits>

namespace Luth
{
    // =============================================
    //            Primitive Types
    // =============================================
    using i8 = int8_t;      // 8-bit signed integer
    using i16 = int16_t;    // 16-bit signed integer
    using i32 = int32_t;    // 32-bit signed integer
    using i64 = int64_t;    // 64-bit signed integer

    using u8 = uint8_t;     // 8-bit unsigned integer
    using u16 = uint16_t;   // 16-bit unsigned integer
    using u32 = uint32_t;   // 32-bit unsigned integer
    using u64 = uint64_t;   // 64-bit unsigned integer

    using f32 = float;      // 32-bit floating point
    using f64 = double;     // 64-bit floating point

    // =============================================
    //              GLM Integrations
    // =============================================
    // Alias GLM types for engine consistency
    using Vec2 = glm::vec2;
    using Vec3 = glm::vec3;
    using Vec4 = glm::vec4;

    using Mat3 = glm::mat3;
    using Mat4 = glm::mat4;

    using Quat = glm::quat;

    // =============================================
    //              Math Constants
    // =============================================
    constexpr f32 PI = 3.14159265358979323846f;
    constexpr f32 TWO_PI = 2.0f * PI;
    constexpr f32 HALF_PI = 0.5f * PI;
    constexpr f32 EPSILON = std::numeric_limits<f32>::epsilon();
    constexpr f32 FLOAT_MAX = std::numeric_limits<f32>::max();
    constexpr f32 FLOAT_MIN = std::numeric_limits<f32>::min();

    // =============================================
    //              Type Traits
    // =============================================
    // Check if a type is a GLM vector
    template<typename T>
    struct IsGLMVector : std::false_type {};

    template<glm::length_t L, typename T, glm::qualifier Q>
    struct IsGLMVector<glm::vec<L, T, Q>> : std::true_type {};

    // Check if a type is a GLM matrix
    template<typename T>
    struct IsGLMMatrix : std::false_type {};

    template<glm::length_t C, glm::length_t R, typename T, glm::qualifier Q>
    struct IsGLMMatrix<glm::mat<C, R, T, Q>> : std::true_type {};

    // =============================================
    //              Forward Declarations
    // =============================================
    // Forward-declare common GLM functions
    template<typename T>
    T Normalize(const T& v);

    template<typename T>
    T Cross(const T& a, const T& b);

}

// =============================================
//          Static Assertions (Safety)
// =============================================
static_assert(sizeof(Luth::i32) == 4, "i32 must be 4 bytes!");
static_assert(sizeof(Luth::f32) == 4, "f32 must be 4 bytes!");
static_assert(sizeof(Luth::Vec3) == 12, "Vec3 must be 12 bytes!");

// =============================================
//           Custom Formatters (Logging)
// =============================================
#include <spdlog/fmt/ostr.h>  // For operator<< overloading

// Format glm::vec3 for logging
inline std::ostream& operator<<(std::ostream& os, const glm::vec3& v) {
    return os << "(" << v.x << ", " << v.y << ", " << v.z << ")";
}

// Format glm::mat4 for logging (simplified)
inline std::ostream& operator<<(std::ostream& os, const glm::mat4& m) {
    for (int i = 0; i < 4; ++i) {
        os << "\n| ";
        for (int j = 0; j < 4; ++j)
            os << m[i][j] << " ";
    }
    return os << " |";
}
