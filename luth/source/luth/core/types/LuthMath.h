#pragma once

#define GLM_FORCE_RADIANS
#define GLM_FORCE_DEPTH_ZERO_TO_ONE
#define GLM_ENABLE_EXPERIMENTAL

#include "luth/core/types/LuthTypes.h"

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/gtx/quaternion.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <glm/gtx/matrix_decompose.hpp>
#include <glm/gtx/euler_angles.hpp>
#include <glm/gtx/norm.hpp>

#include <assimp/matrix3x3.h>
#include <assimp/matrix4x4.h>
#include <assimp/quaternion.h>

#include <array>
#include <limits>
#include <numbers>

namespace Luth
{
    // ---- GLM Type Aliases ----
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

    // ---- Static Assertions (Safety) ----
    static_assert(sizeof(Luth::Vec2)  == 8,  "Vec2 must be 8 bytes!");
    static_assert(sizeof(Luth::Vec3)  == 12, "Vec3 must be 12 bytes!");
    static_assert(sizeof(Luth::Vec4)  == 16, "Vec4 must be 16 bytes!");
    static_assert(sizeof(Luth::IVec2) == 8,  "IVec2 must be 8 bytes!");
    static_assert(sizeof(Luth::IVec4) == 16, "IVec4 must be 16 bytes!");
    static_assert(sizeof(Luth::UVec2) == 8,  "UVec2 must be 8 bytes!");
    static_assert(sizeof(Luth::Mat3)  == 36, "Mat3 must be 36 bytes!");
    static_assert(sizeof(Luth::Mat4)  == 64, "Mat4 must be 64 bytes!");
    static_assert(sizeof(Luth::Quat)  == 16, "Quat must be 16 bytes!");
}

namespace Luth::Math
{
    // ---- GLM trait re-exports ----
    using length_t  = glm::length_t;
    using qualifier = glm::qualifier;

    // ---- Constants ----
    // Standard constants: delegate to <numbers>
    template<class T> inline constexpr T Pi              = std::numbers::pi_v<T>;
    template<class T> inline constexpr T E               = std::numbers::e_v<T>;
    template<class T> inline constexpr T Sqrt2           = std::numbers::sqrt2_v<T>;

    // Derived / engine-specific
    template<class T> inline constexpr T TwoPi           = T(2) * Pi<T>;
    template<class T> inline constexpr T HalfPi          = Pi<T> / T(2);
    template<class T> inline constexpr T QuarterPi       = Pi<T> / T(4);
    template<class T> inline constexpr T InvPi           = T(1) / Pi<T>;
    template<class T> inline constexpr T DegToRad        = Pi<T> / T(180);
    template<class T> inline constexpr T RadToDeg        = T(180) / Pi<T>;

    // Tolerances (no std equivalent)
    template<class T> inline constexpr T SmallNumber      = T(1e-8);  // treat-as-zero threshold for spatial math
    template<class T> inline constexpr T KindaSmallNumber = T(1e-4);  // vector-comparison tolerance

    // Limit sentinels: delegate to <limits>
    template<class T> inline constexpr T FloatMax        = std::numeric_limits<T>::max();
    template<class T> inline constexpr T FloatLowest     = std::numeric_limits<T>::lowest();   // -max for floats
    template<class T> inline constexpr T FloatMin        = std::numeric_limits<T>::min();      // smallest positive finite
    template<class T> inline constexpr T MachineEpsilon  = std::numeric_limits<T>::epsilon();  // ULP-level epsilon

    // ---- Transformation matrices ----
    inline Mat4 Translate(const Mat4& m, const Vec3& v)                                 { return glm::translate(m, v); }
    inline Mat4 Rotate(const Mat4& m, f32 angle, const Vec3& axis)                      { return glm::rotate(m, angle, axis); }
    inline Mat4 Scale(const Mat4& m, const Vec3& v)                                     { return glm::scale(m, v); }
    inline Mat4 Perspective(f32 fovy, f32 aspect, f32 zNear, f32 zFar)                  { return glm::perspective(fovy, aspect, zNear, zFar); }
    inline Mat4 Ortho(f32 left, f32 right, f32 bottom, f32 top, f32 zNear, f32 zFar)    { return glm::ortho(left, right, bottom, top, zNear, zFar); }
    inline Mat4 Ortho(f32 left, f32 right, f32 bottom, f32 top)                         { return glm::ortho(left, right, bottom, top); }
    inline Mat4 LookAt(const Vec3& eye, const Vec3& center, const Vec3& up)             { return glm::lookAt(eye, center, up); }

    // ---- Linear algebra ----
    template<class T> inline T    Inverse(const T& v)                                   { return glm::inverse(v); }
    template<class T> inline T    Transpose(const T& v)                                 { return glm::transpose(v); }
    template<class T> inline T    Normalize(const T& v)                                 { return glm::normalize(v); }
    template<class T> inline auto Length(const T& v)                                    { return glm::length(v); }
    template<class T> inline auto Length2(const T& v)                                   { return glm::length2(v); }
    template<class T> inline auto Dot(const T& a, const T& b)                           { return glm::dot(a, b); }
    template<class T> inline T    Cross(const T& a, const T& b)                         { return glm::cross(a, b); }

    // ---- Interpolation ----
    template<class T, class U> inline T Mix(const T& a, const T& b, const U& t)         { return glm::mix(a, b, t); }
    inline Quat Slerp(const Quat& x, const Quat& y, f32 a)                              { return glm::slerp(x, y, a); }

    // ---- Quaternion utilities ----
    inline Mat4 ToMat4(const Quat& q)                                                   { return glm::toMat4(q); }
    inline Vec3 EulerAngles(const Quat& q)                                              { return glm::eulerAngles(q); }
    inline Quat QuatLookAt(const Vec3& direction, const Vec3& up)                       { return glm::quatLookAt(direction, up); }

    // Full matrix decomposition (translation/rotation/scale + skew/perspective).
    // Use Luth::DecomposeTransform for the trs-only case.
    inline bool Decompose(const Mat4& m, Vec3& scale, Quat& rotation, Vec3& translation,
                          Vec3& skew, Vec4& perspective)
    {
        return glm::decompose(m, scale, rotation, translation, skew, perspective);
    }

    // ---- Angle conversions ----
    template<class T> inline T Radians(const T& v)                                      { return glm::radians(v); }
    template<class T> inline T Degrees(const T& v)                                      { return glm::degrees(v); }
    template<class T> inline T Cos(const T& v)                                          { return glm::cos(v); }

    // ---- Common math (scalar + vector) ----
    template<class T, class U> inline T Clamp(const T& v, const U& lo, const U& hi)     { return glm::clamp(v, lo, hi); }
    template<class T> inline T Min(const T& a, const T& b)                              { return glm::min(a, b); }
    template<class T> inline T Max(const T& a, const T& b)                              { return glm::max(a, b); }
    template<class T> inline T Abs(const T& v)                                          { return glm::abs(v); }

    // ---- Pointer helpers ----
    template<class T> inline auto*       ValuePtr(T& v)                                 { return glm::value_ptr(v); }
    template<class T> inline const auto* ValuePtr(const T& v)                           { return glm::value_ptr(v); }
    inline Vec3 MakeVec3(const f32* p)                                                  { return glm::make_vec3(p); }
}

namespace Luth
{
    // ---- Assimp <-> Luth conversions ----
    inline Mat3 AiMat3ToGLM(const aiMatrix3x3& from) {
        return {
            {from.a1, from.b1, from.c1},
            {from.a2, from.b2, from.c2},
            {from.a3, from.b3, from.c3}
        };
    }

    inline Mat4 AiMat4ToGLM(const aiMatrix4x4& from) {
        return {
            {from.a1, from.b1, from.c1, from.d1},
            {from.a2, from.b2, from.c2, from.d2},
            {from.a3, from.b3, from.c3, from.d3},
            {from.a4, from.b4, from.c4, from.d4}
        };
    }

    inline Vec3 AiVec3ToGLM(const aiVector3D& v) {
        return { v.x, v.y, v.z };
    }

    inline Quat AiQuatToGLM(const aiQuaternion& q) {
        return { q.w, q.x, q.y, q.z };
    }

    inline aiMatrix3x3 GLMMat3ToAi(const Mat3& m) {
        return {
            m[0][0], m[1][0], m[2][0],
            m[0][1], m[1][1], m[2][1],
            m[0][2], m[1][2], m[2][2]
        };
    }

    inline aiMatrix4x4 GLMMat4ToAi(const Mat4& m) {
        return {
            m[0][0], m[1][0], m[2][0], m[3][0],
            m[0][1], m[1][1], m[2][1], m[3][1],
            m[0][2], m[1][2], m[2][2], m[3][2],
            m[0][3], m[1][3], m[2][3], m[3][3]
        };
    }

    inline aiVector3D GLMVec3ToAi(const Vec3& v) {
        return { v.x, v.y, v.z };
    }

    inline aiQuaternion GLMQuatToAi(const Quat& q) {
        return { q.w, q.x, q.y, q.z };
    }

    // ---- Matrix helpers ----
    inline Mat3 ConvertToNormalMatrix(const Mat4& modelMatrix) {
        return Math::Transpose(Math::Inverse(Mat3(modelMatrix)));
    }

    inline Mat3 Mat4ToMat3(const Mat4& m) {
        return Mat3(m);
    }

    // ---- Transform compose / decompose ----
    inline Mat4 ComposeTransform(
        const Vec3& translation,
        const Quat& rotation,
        const Vec3& scale)
    {
        const Mat4 t = Math::Translate(Mat4(1.0f), translation);
        const Mat4 r = Math::ToMat4(rotation);
        const Mat4 s = Math::Scale(Mat4(1.0f), scale);
        return t * r * s;
    }

    inline void DecomposeTransform(
        const Mat4& transform,
        Vec3& translation,
        Quat& rotation,
        Vec3& scale)
    {
        Vec3 skew;
        Vec4 perspective;
        glm::decompose(transform, scale, rotation, translation, skew, perspective);
        rotation = glm::conjugate(rotation);
    }

    // ---- Axis-Aligned Bounding Box ----
    struct AABB {
        Vec3 Min = Vec3( Math::FloatMax<f32>);
        Vec3 Max = Vec3(Math::FloatLowest<f32>);

        void Expand(const Vec3& point) {
            Min = Math::Min(Min, point);
            Max = Math::Max(Max, point);
        }
        void Expand(const AABB& other) {
            Min = Math::Min(Min, other.Min);
            Max = Math::Max(Max, other.Max);
        }
        Vec3 Center() const { return (Min + Max) * 0.5f; }
        Vec3 Extents() const { return (Max - Min) * 0.5f; }
        bool IsValid() const { return Min.x <= Max.x; }
    };

    // ---- Frustum culling ----
    struct Frustum {
        std::array<Vec4, 6> planes;
    };

    inline Frustum CreateFrustumFromCamera(const Mat4& viewProj, bool normalize = true) {
        Frustum frustum;
        const Mat4 matrix = Math::Transpose(viewProj);

        frustum.planes[0] = matrix[3] + matrix[0]; // Left
        frustum.planes[1] = matrix[3] - matrix[0]; // Right
        frustum.planes[2] = matrix[3] + matrix[1]; // Bottom
        frustum.planes[3] = matrix[3] - matrix[1]; // Top
        frustum.planes[4] = matrix[3] + matrix[2]; // Near
        frustum.planes[5] = matrix[3] - matrix[2]; // Far

        if (normalize) {
            for (auto& plane : frustum.planes) {
                const f32 length = Math::Length(Vec3(plane));
                plane /= length;
            }
        }

        return frustum;
    }

    inline bool IsInFrustum(const Frustum& frustum, const Vec3& point, f32 radius = 0.0f) {
        for (const auto& plane : frustum.planes) {
            const f32 distance =
                plane.x * point.x +
                plane.y * point.y +
                plane.z * point.z +
                plane.w;

            if (distance < -radius)
                return false;
        }
        return true;
    }
}
