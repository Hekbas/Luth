#include "luthpch.h"

#include "luth/core/DebugDraw.h"

#include <atomic>
#include <cmath>
#include <vector>

namespace Luth::DebugDraw
{
    namespace
    {
        // Two slots cycled by frame index. Modulo-2 is enough because at any instant the engine
        // holds at most two frames whose debug-draw matters: the game frame currently writing and
        // the render frame currently reading. The GPU frame two iterations out only consumes
        // already-recorded command buffers; it doesn't touch DebugDraw state.
        std::vector<DebugVertex> s_Buffers[2];

        // Producer slot index (mod 2). Updated by BeginGameFrame; read by Line/Triangle on the
        // game-stage fiber. Relaxed atomic is enough: BeginGameFrame happens-before the Game stage
        // dispatch (both on the main thread) which itself synchronizes-with the worker fiber via
        // the JobSystem's queue handoff.
        std::atomic<u32> s_ProducerSlot { 0 };

        // Orthonormal basis (u, v) spanning the plane perpendicular to a unit axis n. Reference axis
        // is swapped near the poles so the cross product never degenerates.
        void BasisFromAxis(const Vec3& n, Vec3& u, Vec3& v)
        {
            const Vec3 ref = (std::abs(n.x) < 0.9f) ? Vec3(1.0f, 0.0f, 0.0f) : Vec3(0.0f, 1.0f, 0.0f);
            u = Math::Normalize(Math::Cross(ref, n));
            v = Math::Cross(n, u);
        }
    }

    void Init()
    {
        for (auto& buf : s_Buffers)
        {
            buf.clear();
            buf.reserve(4096);
        }
        s_ProducerSlot.store(0, std::memory_order_relaxed);
    }

    void Shutdown()
    {
        for (auto& buf : s_Buffers)
        {
            buf.clear();
            buf.shrink_to_fit();
        }
    }

    void BeginGameFrame(u64 gameFrameIdx)
    {
        const u32 slot = static_cast<u32>(gameFrameIdx & 1ull);
        s_ProducerSlot.store(slot, std::memory_order_relaxed);
        s_Buffers[slot].clear();
    }

    void Line(const Vec3& from, const Vec3& to, u32 colorRGBA)
    {
        const u32 slot = s_ProducerSlot.load(std::memory_order_relaxed);
        auto& buf = s_Buffers[slot];
        buf.push_back({ from, colorRGBA });
        buf.push_back({ to,   colorRGBA });
    }

    void Triangle(const Vec3& v0, const Vec3& v1, const Vec3& v2, u32 colorRGBA)
    {
        Line(v0, v1, colorRGBA);
        Line(v1, v2, colorRGBA);
        Line(v2, v0, colorRGBA);
    }

    void WireBox(const Mat4& transform, const Vec3& localMin, const Vec3& localMax, u32 color)
    {
        Vec3 c[8];
        for (int i = 0; i < 8; ++i)
        {
            const Vec3 local((i & 1) ? localMax.x : localMin.x,
                             (i & 2) ? localMax.y : localMin.y,
                             (i & 4) ? localMax.z : localMin.z);
            c[i] = Vec3(transform * Vec4(local, 1.0f));
        }
        // Corner index bits = (x, y, z); each edge joins corners differing in exactly one bit.
        Line(c[0], c[1], color); Line(c[2], c[3], color); Line(c[4], c[5], color); Line(c[6], c[7], color);
        Line(c[0], c[2], color); Line(c[1], c[3], color); Line(c[4], c[6], color); Line(c[5], c[7], color);
        Line(c[0], c[4], color); Line(c[1], c[5], color); Line(c[2], c[6], color); Line(c[3], c[7], color);
    }

    void WireSphere(const Vec3& center, float radius, u32 color, int segments)
    {
        // Three orthogonal great circles: the Unity-style point-light/range gizmo.
        const float step = Math::TwoPi<float> / float(segments);
        for (int axis = 0; axis < 3; ++axis)
        {
            for (int s = 0; s < segments; ++s)
            {
                const float a0 = step * float(s), a1 = step * float(s + 1);
                const float c0 = std::cos(a0), s0 = std::sin(a0);
                const float c1 = std::cos(a1), s1 = std::sin(a1);
                Vec3 p0, p1;
                switch (axis)
                {
                    case 0:  p0 = center + Vec3(c0, s0, 0.0f) * radius; p1 = center + Vec3(c1, s1, 0.0f) * radius; break;
                    case 1:  p0 = center + Vec3(c0, 0.0f, s0) * radius; p1 = center + Vec3(c1, 0.0f, s1) * radius; break;
                    default: p0 = center + Vec3(0.0f, c0, s0) * radius; p1 = center + Vec3(0.0f, c1, s1) * radius; break;
                }
                Line(p0, p1, color);
            }
        }
    }

    void WireCircle(const Vec3& center, const Vec3& axis, float radius, u32 color, int segments)
    {
        Vec3 u, v;
        BasisFromAxis(Math::Normalize(axis), u, v);
        const float step = Math::TwoPi<float> / float(segments);
        Vec3 prev = center + u * radius;
        for (int s = 1; s <= segments; ++s)
        {
            const float a = step * float(s);
            const Vec3 p = center + (u * std::cos(a) + v * std::sin(a)) * radius;
            Line(prev, p, color);
            prev = p;
        }
    }

    void WireFrustum(const Vec3 corners[8], u32 color)
    {
        for (int i = 0; i < 4; ++i) Line(corners[i],     corners[(i + 1) % 4],     color);     // near quad
        for (int i = 0; i < 4; ++i) Line(corners[4 + i], corners[4 + (i + 1) % 4], color);     // far quad
        for (int i = 0; i < 4; ++i) Line(corners[i],     corners[4 + i],           color);     // connectors
    }

    void WireCone(const Vec3& apex, const Vec3& dir, float height, float baseRadius, u32 color, int segments)
    {
        const Vec3 n = Math::Normalize(dir);
        const Vec3 baseCenter = apex + n * height;
        Vec3 u, v;
        BasisFromAxis(n, u, v);
        const float step = Math::TwoPi<float> / float(segments);
        Vec3 prev = baseCenter + u * baseRadius;
        for (int s = 1; s <= segments; ++s)
        {
            const float a = step * float(s);
            const Vec3 p = baseCenter + (u * std::cos(a) + v * std::sin(a)) * baseRadius;
            Line(prev, p, color);
            prev = p;
        }
        // Four spokes apex -> base ring at the quarter points.
        for (int k = 0; k < 4; ++k)
        {
            const float a = Math::TwoPi<float> * float(k) / 4.0f;
            Line(apex, baseCenter + (u * std::cos(a) + v * std::sin(a)) * baseRadius, color);
        }
    }

    void Arrow(const Vec3& from, const Vec3& to, u32 color, float headSize)
    {
        Line(from, to, color);
        Vec3 dir = to - from;
        const float len = Math::Length(dir);
        if (len < 1e-4f) return;
        dir /= len;
        Vec3 u, v;
        BasisFromAxis(dir, u, v);
        const Vec3 base = to - dir * headSize;
        const float r = headSize * 0.5f;
        Line(to, base + u * r, color);
        Line(to, base - u * r, color);
        Line(to, base + v * r, color);
        Line(to, base - v * r, color);
    }

    void Cross(const Vec3& center, float size, u32 color)
    {
        Line(center - Vec3(size, 0.0f, 0.0f), center + Vec3(size, 0.0f, 0.0f), color);
        Line(center - Vec3(0.0f, size, 0.0f), center + Vec3(0.0f, size, 0.0f), color);
        Line(center - Vec3(0.0f, 0.0f, size), center + Vec3(0.0f, 0.0f, size), color);
    }

    std::span<const DebugVertex> GetForRender(u64 renderFrameIdx)
    {
        const u32 slot = static_cast<u32>(renderFrameIdx & 1ull);
        return s_Buffers[slot];
    }
}
