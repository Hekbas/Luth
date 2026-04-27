#pragma once

#include "luth/core/types/LuthTypes.h"
#include "luth/core/diagnostics/Log.h"
#include <atomic>
#include <cassert>

#ifdef _WIN32
#include <windows.h>
#endif

namespace Luth::JobSystem
{
    // Forward declare — implemented in JobSystem.cpp via FLS
    struct JobContext;
    JobContext* GetCurrentJobContext();

    struct Fiber
    {
        void* Handle = nullptr;
        void* Args = nullptr; // User data passed to the fiber function

        // Intrusive Linked List for Waiting (Lock-Free Stack Node)
        Fiber* NextWaiting = nullptr;

        // Pinning Support
        u32 PinnedThreadIndex = ~0u; // Default: No affinity

        // Status
        bool IsFinished = false;

        // State for Race Condition Prevention
        // 0 = Free, 1 = Running, 2 = Suspended
        std::atomic<u8> State;

        // Wait Request (Passed to Scheduler)
        void* WaitCounter = nullptr;
        u32 WaitTarget = 0;

        // Default Constructor
        Fiber() : State(0) {}

        // Move Constructor
        Fiber(Fiber&& other) noexcept
        {
            Handle = other.Handle;
            Args = other.Args;
            NextWaiting = other.NextWaiting;
            PinnedThreadIndex = other.PinnedThreadIndex;
            IsFinished = other.IsFinished;
            State.store(other.State.load());
            WaitCounter = other.WaitCounter;
            WaitTarget = other.WaitTarget;

            other.Handle = nullptr;
        }

        // Move Assignment
        Fiber& operator=(Fiber&& other) noexcept
        {
            if (this != &other)
            {
                Handle = other.Handle;
                Args = other.Args;
                NextWaiting = other.NextWaiting;
                PinnedThreadIndex = other.PinnedThreadIndex;
                IsFinished = other.IsFinished;
                State.store(other.State.load());
                WaitCounter = other.WaitCounter;
                WaitTarget = other.WaitTarget;

                other.Handle = nullptr;
            }
            return *this;
        }

        // Deleted Copy Constructor & Assignment
        Fiber(const Fiber&) = delete;
        Fiber& operator=(const Fiber&) = delete;

        bool IsPinned() const { return PinnedThreadIndex != ~0u; }

        // Function pointer for the fiber entry point
        using EntryPoint = void(*)(void*);

        // 2MB stack — handles heavy asset importers (Assimp)
        static Fiber Create(EntryPoint entry, void* args, u32 stackSize = 2 * 1024 * 1024)
        {
            Fiber f;
            f.Args = args;
            f.NextWaiting = nullptr;
            f.PinnedThreadIndex = ~0u;
            f.IsFinished = false;
            f.State = 0;
            f.WaitCounter = nullptr;
            f.WaitTarget = 0;

            #ifdef _WIN32
            // FIBER_FLAG_FLOAT_SWITCH: preserve x87/MMX/XMM state across fiber switches.
            f.Handle = CreateFiberEx(64 * 1024, stackSize, FIBER_FLAG_FLOAT_SWITCH, (LPFIBER_START_ROUTINE)entry, args);
            if (!f.Handle)
            {
                LH_CORE_CRITICAL("Failed to create fiber! Error: {0}", GetLastError());
            }
            #endif

            return f;
        }

        static void Destroy(Fiber& f)
        {
            #ifdef _WIN32
            if (f.Handle) DeleteFiber(f.Handle);
            #endif
            f.Handle = nullptr;
        }

        // V3 ENFORCEMENT: Assert that we are NOT inside a RecordingScope.
        // If this assertion fires, someone is yielding while recording a VkCommandBuffer.
        // That violates Contract 4 (VkCommandPool is thread-local, fiber may resume
        // on a different OS thread after yield).
        static void SwitchTo(Fiber& f)
        {
            #ifndef NDEBUG
            {
                JobContext* ctx = GetCurrentJobContext();
                if (ctx)
                {
                    assert(!ctx->IsRecording &&
                        "FATAL: Fiber::SwitchTo called while IsRecording == true! "
                        "You are yielding inside a RecordingScope. This violates Contract 4 "
                        "(VkCommandBuffer thread affinity). End recording before yielding.");
                }
            }
            #endif

            #ifdef _WIN32
            SwitchToFiber(f.Handle);
            #endif
        }

        static Fiber ConvertThreadToFiber(void* args)
        {
            Fiber f;
            f.Args = args;
            f.NextWaiting = nullptr;
            f.PinnedThreadIndex = ~0u;
            f.IsFinished = false;
            f.State = 1; // Running
            f.WaitCounter = nullptr;
            f.WaitTarget = 0;
            #ifdef _WIN32
            if (IsThreadAFiber())
                f.Handle = GetCurrentFiber();
            else
                f.Handle = ::ConvertThreadToFiberEx(args, FIBER_FLAG_FLOAT_SWITCH);
            #endif
            return f;
        }
    };
}
