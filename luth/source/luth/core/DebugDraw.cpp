#include "luthpch.h"

#include "luth/core/DebugDraw.h"

#include <atomic>
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

        // Producer slot index (mod 2). Updated by BeginGameFrame — read by Line/Triangle on the
        // game-stage fiber. Relaxed atomic is enough: BeginGameFrame happens-before the Game stage
        // dispatch (both on the main thread) which itself synchronizes-with the worker fiber via
        // the JobSystem's queue handoff.
        std::atomic<u32> s_ProducerSlot { 0 };
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

    std::span<const DebugVertex> GetForRender(u64 renderFrameIdx)
    {
        const u32 slot = static_cast<u32>(renderFrameIdx & 1ull);
        return s_Buffers[slot];
    }
}
