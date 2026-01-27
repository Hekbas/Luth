#pragma once

#include "luth/core/LuthTypes.h"
#include "luth/core/Log.h"
#include <atomic>

#ifdef _WIN32
#include <windows.h>
#endif

namespace Luth::JobSystem
{
    struct Fiber
    {
        void* Handle = nullptr;
        void* Args = nullptr; // User data passed to the fiber function
        
        // Intrusive Linked List for Waiting (Lock-Free Stack Node)
        // Used when this fiber is waiting on an AtomicCounter
        Fiber* NextWaiting = nullptr;
        
        // Pinning Support
        u32 PinnedThreadIndex = ~0u; // Default: No affinity (~0u)
        
        // Status
        bool IsFinished = false;
        
        // State for Race Condition Prevention
        // 0 = Free
        // 1 = Running (Do not switch to this)
        // 2 = Suspended (Safe to switch to)
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

        // Deleted Copy Constructor & Assignment (std::atomic is not copyable)
        Fiber(const Fiber&) = delete;
        Fiber& operator=(const Fiber&) = delete;

        bool IsPinned() const { return PinnedThreadIndex != ~0u; }

        // Function pointer for the fiber entry point
        using EntryPoint = void(*)(void*);
        
        // Increased stack size to 2MB to handle heavy asset importers (Assimp)
        static Fiber Create(EntryPoint entry, void* args, u32 stackSize = 2 * 1024 * 1024)
        {
            Fiber f;
            f.Args = args;
            f.NextWaiting = nullptr;
            f.PinnedThreadIndex = ~0u;
            f.IsFinished = false;
            f.State = 0; // Free
            f.WaitCounter = nullptr;
            f.WaitTarget = 0;

            #ifdef _WIN32
            // Commit 64KB initially, Reserve full size.
            f.Handle = CreateFiberEx(64 * 1024, stackSize, 0, (LPFIBER_START_ROUTINE)entry, args);
            
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

        static void SwitchTo(Fiber& f)
        {
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
            f.State = 1; // Running (Main Thread is always running)
            f.WaitCounter = nullptr;
            f.WaitTarget = 0;
            #ifdef _WIN32
            if (IsThreadAFiber())
            {
                 f.Handle = GetCurrentFiber();
            }
            else
            {
                f.Handle = ::ConvertThreadToFiber(args);
            }
            #endif
            return f;
        }
    };
}
