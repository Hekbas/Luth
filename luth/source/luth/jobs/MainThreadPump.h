#pragma once

#include "luth/core/types/LuthTypes.h"

#include <functional>

namespace Luth
{
    // Worker-fiber → main-thread callback hop. Async editor work that needs to
    // touch ImGui on completion posts a callback here; App::Run drains once
    // per frame between EventBus::ProcessEvents and the editor BeginFrame so
    // callbacks mutate state, not ImGui mid-frame.

    // Edge frequency by design — autosave / thumbnail consumers fire seconds
    // apart, not per-frame. std::mutex + std::queue mirror EventBus's shape.
    class MainThreadPump
    {
    public:
        using Callback = std::function<void()>;

        // Post a callback for main-thread execution on the next Drain. Safe
        // from any thread / fiber. Callback must be non-blocking (no
        // JobSystem::WaitForCounter, no OS sync). Re-Posting from inside a
        // callback lands in the next frame's drain, not the current one.
        static void Post(Callback cb);

        // Drain and run all pending callbacks. Single-thread-only by design;
        // first call latches the calling thread (debug-only assert).
        static void Drain();

        // Diagnostic — pending callback count. Atomic load.
        static u32 PendingCount();
    };
}
