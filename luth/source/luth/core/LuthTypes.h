#pragma once

#include <cstdint>
#include <type_traits>
#include <filesystem>
#include <spdlog/fmt/ostr.h>
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

namespace Luth
{
    // =============================================
    //                   Alias
    // =============================================
    namespace fs = std::filesystem;

    // =============================================
    //            Primitive Types
    // =============================================
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

    // =============================================
    //              GLM Integrations
    // =============================================
    using Vec2 = glm::vec2;
    using Vec3 = glm::vec3;
    using Vec4 = glm::vec4;

    using IVec2 = glm::ivec2;
    using IVec3 = glm::ivec3;
    using IVec4 = glm::ivec4;

    using UVec2 = glm::uvec2;
    using UVec3 = glm::uvec3;
    using UVec4 = glm::uvec4;

    using Mat2 = glm::mat2;
    using Mat3 = glm::mat3;
    using Mat4 = glm::mat4;

    using Quat = glm::quat;

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
    // Common GLM functions
    template<typename T>
    T Normalize(const T& v);

    template<typename T>
    T Cross(const T& a, const T& b);

    // =============================================
    //          Static Assertions (Safety)
    // =============================================
    static_assert(sizeof(Luth::i32)   == 4,  "i32 must be 4 bytes!");
    static_assert(sizeof(Luth::f32)   == 4,  "f32 must be 4 bytes!");
    static_assert(sizeof(Luth::Vec2)  == 8,  "Vec2 must be 8 bytes!");
    static_assert(sizeof(Luth::Vec3)  == 12, "Vec3 must be 12 bytes!");
    static_assert(sizeof(Luth::Vec4)  == 16, "Vec4 must be 16 bytes!");
    static_assert(sizeof(Luth::IVec2) == 8,  "IVec2 must be 8 bytes!");
    static_assert(sizeof(Luth::IVec4) == 16, "IVec4 must be 16 bytes!");
    static_assert(sizeof(Luth::UVec2) == 8,  "UVec2 must be 8 bytes!");
    static_assert(sizeof(Luth::Mat3)  == 36, "Mat3 must be 36 bytes!");
    static_assert(sizeof(Luth::Mat4)  == 64, "Mat4 must be 64 bytes!");
    static_assert(sizeof(Luth::Quat)  == 16, "Quat must be 16 bytes!");

    // =============================================
    //           Custom Formatters (Logging)
    // =============================================   
    // Format glm::vec3
    inline std::ostream& operator<<(std::ostream& os, const glm::vec3& v) {
        return os << "(" << v.x << ", " << v.y << ", " << v.z << ")";
    }

    // Format glm::mat4
    inline std::ostream& operator<<(std::ostream& os, const glm::mat4& m) {
        for (int i = 0; i < 4; ++i) {
            os << "\n| ";
            for (int j = 0; j < 4; ++j)
                os << m[i][j] << " ";
        }
        return os << " |";
    }
}
