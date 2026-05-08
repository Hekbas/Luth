#pragma once

#include "luth/core/types/LuthMath.h"
#include "luth/core/types/LuthTypes.h"

#include <span>

namespace Luth
{
    // GPU-friendly line endpoint. Color is packed RGBA8 (byte 0 = R, byte 3 = A) so the renderer
    // can bind the buffer directly with VK_FORMAT_R8G8B8A8_UNORM and skip a per-vertex unpack.
    struct DebugVertex
    {
        Vec3 position;
        u32  colorRGBA;
    };
    static_assert(sizeof(DebugVertex) == 16, "DebugVertex expected 16 bytes");
}

namespace Luth::DebugDraw
{
    // Shared, low-dependency facility for ad-hoc line visualization. Producers (game-stage code
    // such as PhysicsSystem) push line endpoints; consumers (the renderer's DebugDrawSubsystem)
    // read them once per frame. Frame-indexed double-buffering: each frame writes to slot
    // (frameIndex mod 2), the previous frame's slot stays valid for the render stage to read.
    //
    // Thread model: the game stage runs as one fiber that owns the producer slot for that frame —
    // single-writer, no locking. Render reads the slot whose game writer has already finished (the
    // main thread waits on GameReady before that slot is reused), so the read side is also
    // race-free as long as BeginGameFrame is only called from the main thread between frames.

    void Init();
    void Shutdown();

    // Called once per frame on the main thread before the Game stage dispatches. Selects the
    // producer slot via gameFrameIdx mod 2 and clears it. Subsequent Line/Triangle calls write
    // into that slot.
    void BeginGameFrame(u64 gameFrameIdx);

    // Producer API. Game-stage callers append line segments. Triangle decomposes to three lines —
    // a separate filled-triangle path will land if/when we need solid debug shapes.
    void Line(const Vec3& from, const Vec3& to, u32 colorRGBA);
    void Triangle(const Vec3& v0, const Vec3& v1, const Vec3& v2, u32 colorRGBA);

    // Consumer API. Returns the slot for the requested render frame (renderFrameIdx mod 2).
    // The span is valid until BeginGameFrame is called again for the same modulo-2 slot — the
    // frame pipeline guarantees that's at least two frames out.
    std::span<const DebugVertex> GetForRender(u64 renderFrameIdx);
}
