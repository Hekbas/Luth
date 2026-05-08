#pragma once

// Jolt's umbrella header sets up the JPH:: namespace and config macros — must
// be included before any other Jolt header.
#include <Jolt/Jolt.h>

#include <Jolt/Math/Vec3.h>
#include <Jolt/Math/Quat.h>
#include <Jolt/Math/Mat44.h>
#include <Jolt/Math/Float4.h>

#include "luth/core/types/LuthMath.h"

namespace Luth::Physics
{
    // GLM and Jolt share Mat4/Mat44 column-major layout and Quat (x,y,z,w)
    // storage, so matrix and quaternion conversions are bitwise-cheap.
    // Vec3 differs in size (GLM 12 B / 3 floats vs Jolt 16 B / 4-wide SSE),
    // so vector conversions go through accessors.

    // ── Vec3 ──
    inline JPH::Vec3 ToJolt(const Vec3& v)
    {
        return JPH::Vec3(v.x, v.y, v.z);
    }

    inline Vec3 FromJolt(const JPH::Vec3& v)
    {
        return Vec3(v.GetX(), v.GetY(), v.GetZ());
    }

    // ── Quat ──
    inline JPH::Quat ToJolt(const Quat& q)
    {
        // glm::quat ctor is (w, x, y, z); accessors (.x .y .z .w) match
        // component names. Jolt's Quat ctor is (x, y, z, w).
        return JPH::Quat(q.x, q.y, q.z, q.w);
    }

    inline Quat FromJolt(const JPH::Quat& q)
    {
        return Quat(q.GetW(), q.GetX(), q.GetY(), q.GetZ());
    }

    // ── Mat4 ──
    inline JPH::Mat44 ToJolt(const Mat4& m)
    {
        // GLM stores 16 floats contiguously, column-major. Float4 is a 4-float
        // POD; sLoadFloat4x4 is the non-aligned variant — safe regardless of
        // GLM's SIMD alignment configuration.
        return JPH::Mat44::sLoadFloat4x4(
            reinterpret_cast<const JPH::Float4*>(glm::value_ptr(m)));
    }

    inline Mat4 FromJolt(const JPH::Mat44& m)
    {
        Mat4 r;
        m.StoreFloat4x4(reinterpret_cast<JPH::Float4*>(glm::value_ptr(r)));
        return r;
    }

    // Pair these with LuthMath.h's GLM-side asserts to confirm both ends of
    // the conversion. JPH::Vec3 is intentionally larger than its GLM peer.
    static_assert(sizeof(JPH::Vec3)  == 16, "JPH::Vec3 expected 16 bytes (4-wide SSE)");
    static_assert(sizeof(JPH::Quat)  == 16, "JPH::Quat expected 16 bytes");
    static_assert(sizeof(JPH::Mat44) == 64, "JPH::Mat44 expected 64 bytes");
    static_assert(sizeof(JPH::Float4) == 16, "JPH::Float4 expected 16 bytes");
}
