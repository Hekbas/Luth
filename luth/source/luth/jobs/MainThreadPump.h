#pragma once

#include "luth/core/types/LuthTypes.h"

#include <functional>

namespace Luth
{
    // Worker-fiber-to-main-thread callback hop. Async editor work that needs to touch ImGui on
    // completion posts a callback here; App::Run drains once per frame between EventBus
    // ProcessEvents and the editor BeginFrame, so callbacks mutate state and never call ImGui
    // mid-frame. Designed for edge frequency (autosave, thumbnail completions) rather than per-frame
    // traffic; std::mutex + std::queue mirror EventBus's shape because the same constraints apply.
    class MainThreadPump
    {
    public:
        using Callback = std::function<void()>;

        // Post a callback for main-thread execution on the next Drain. Safe to call from any thread
        // or fiber. The callback itself must be non-blocking (no JobSystem::WaitForCounter, no OS
        // sync). Re-posting from inside a callback lands in the NEXT frame's drain, not the current
        // one; same reentrancy contract as EventBus.
        static void Post(Callback cb);

        // Drain and run all pending callbacks. Single-thread-only by design; the first call latches
        // the calling thread and a debug-only assert fires if a different thread tries to drain later.
        static void Drain();

        // Pending-callback count. Atomic load; advisory only.
        static u32 PendingCount();
    };
}
