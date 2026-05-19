#pragma once

#include "luth/core/types/LuthTypes.h"
#include "luth/core/diagnostics/Log.h"
#include "luth/jobs/FiberPrimitive.h"
#include "luth/jobs/FiberStackAllocator.h"

#include <atomic>
#include <cassert>
#include <cstddef>

#ifdef _WIN32
#include <windows.h>
#endif

// ASan fiber-switch annotations. Paired around each jump_fcontext switch so the
// sanitizer can keep its stack-tracking accurate across fiber boundaries. Compiled
// out when ASan is not in the build.
#if defined(__SANITIZE_ADDRESS__)
extern "C" {
    void __sanitizer_start_switch_fiber(void** fake_stack_save,
                                         const void* bottom, size_t size);
    void __sanitizer_finish_switch_fiber(void* fake_stack_save,
                                          const void** bottom_old, size_t* size_old);
}
#endif

namespace Luth::JobSystem
{
    struct JobContext;
    JobContext* GetCurrentJobContext();

    // Worker scheduler fiber wrapper. Stack lives in a VirtualAlloc'd region that we own
    // — bottom/size are known pre-first-switch, which lets ASan track per-fiber bounds
    // (Win32 fibers can't satisfy this). See arch/fiber-system.md for the V1-V6 hazard
    // model and the rationale for the custom backend.
    struct Fiber
    {
        // ── State ──
        void* Args = nullptr;
        Fiber* NextWaiting = nullptr;
        u32 PinnedThreadIndex = ~0u;
        bool IsFinished = false;
        std::atomic<u8> State{0};
        void* WaitCounter = nullptr;
        u32 WaitTarget = 0;

        // ASan tracking: AsanFakeStack is the per-fiber save slot; StackBottom/StackSize
        // are the bounds passed to start_switch_fiber. Populated by Create or
        // CaptureCurrentThreadAsFiber; inert under non-ASan builds.
        void* AsanFakeStack = nullptr;
        void* StackBottom = nullptr;
        size_t StackSize = 0;

        // Saved RSP (initial value from make_fcontext, updated by each jump_fcontext).
        // Stack region owned by Stack — its Region is null for fibers wrapping an existing
        // OS-thread stack via CaptureCurrentThreadAsFiber (we don't own that stack).
        void* Context = nullptr;
        FiberStack Stack{};

        Fiber() : State(0) {}

        Fiber(Fiber&& other) noexcept
        {
            Args = other.Args;
            NextWaiting = other.NextWaiting;
            PinnedThreadIndex = other.PinnedThreadIndex;
            IsFinished = other.IsFinished;
            State.store(other.State.load());
            WaitCounter = other.WaitCounter;
            WaitTarget = other.WaitTarget;
            AsanFakeStack = other.AsanFakeStack;
            StackBottom = other.StackBottom;
            StackSize = other.StackSize;
            Context = other.Context;
            Stack = other.Stack;
            other.Context = nullptr;
            other.Stack = FiberStack{};
        }

        Fiber& operator=(Fiber&& other) noexcept
        {
            if (this != &other)
            {
                Args = other.Args;
                NextWaiting = other.NextWaiting;
                PinnedThreadIndex = other.PinnedThreadIndex;
                IsFinished = other.IsFinished;
                State.store(other.State.load());
                WaitCounter = other.WaitCounter;
                WaitTarget = other.WaitTarget;
                AsanFakeStack = other.AsanFakeStack;
                StackBottom = other.StackBottom;
                StackSize = other.StackSize;
                Context = other.Context;
                Stack = other.Stack;
                other.Context = nullptr;
                other.Stack = FiberStack{};
            }
            return *this;
        }

        Fiber(const Fiber&) = delete;
        Fiber& operator=(const Fiber&) = delete;

        bool IsPinned() const { return PinnedThreadIndex != ~0u; }

        using EntryPoint = void(*)(void*);

        // 2 MB stack — sized for heavy importers (Assimp). ownerCtx is patched into the
        // new fiber's TIB ArbitraryUserPointer save slot so its first resume restores the
        // correct JobContext pointer into gs:[0x28].
        static Fiber Create(EntryPoint entry, void* args, JobContext* ownerCtx,
                             size_t stackSize = 2 * 1024 * 1024)
        {
            Fiber f;
            f.Args = args;
            f.PinnedThreadIndex = ~0u;
            f.IsFinished = false;
            f.State = 0;
            f.WaitCounter = nullptr;
            f.WaitTarget = 0;

            f.Stack = AllocateFiberStack(stackSize);
            if (!f.Stack.UsableBottom)
            {
                LH_CORE_CRITICAL("FiberStack allocation failed; aborting Fiber::Create");
                return f;
            }
            f.StackBottom = f.Stack.UsableBottom;
            f.StackSize = f.Stack.UsableSize;
            f.Context = make_fcontext(f.Stack.StackTop, f.Stack.UsableSize, entry, args);
            // Patch the save area so first jump_fcontext restores ownerCtx into gs:[0x28].
            fcontext_set_owner(f.Context, ownerCtx);

            return f;
        }

        static void Destroy(Fiber& f)
        {
            FreeFiberStack(f.Stack);
            f.Context = nullptr;
            f.StackBottom = nullptr;
            f.StackSize = 0;
        }

        // V3 ENFORCEMENT: Assert that we are NOT inside a RecordingScope.
        // ASan: paired start/finish_switch_fiber tells the sanitizer about the stack
        // swap so it doesn't flag the destination fiber's first stack access as
        // use-after-scope against `from`'s redzones.
        static void SwitchTo(Fiber& from, Fiber& to)
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

            #if defined(__SANITIZE_ADDRESS__)
            __sanitizer_start_switch_fiber(&from.AsanFakeStack,
                                            to.StackBottom, to.StackSize);
            #endif

            jump_fcontext(&from.Context, to.Context);

            #if defined(__SANITIZE_ADDRESS__)
            __sanitizer_finish_switch_fiber(from.AsanFakeStack, nullptr, nullptr);
            #endif
        }

        // Wrap the calling OS thread's existing stack as a Fiber: record bounds (TIB read)
        // and seed gs:[0x28] = ownerCtx so subsequent jump_fcontext save cycles capture
        // the correct per-fiber JobContext pointer.
        static Fiber CaptureCurrentThreadAsFiber(JobContext* ownerCtx)
        {
            Fiber f;
            f.PinnedThreadIndex = ~0u;
            f.IsFinished = false;
            f.State = 1;
            f.WaitCounter = nullptr;
            f.WaitTarget = 0;
            f.CaptureCurrentStackBounds();

            #ifdef _WIN32
            __writegsqword(0x28, reinterpret_cast<uintptr_t>(ownerCtx));
            #endif

            return f;
        }

        // Capture the current OS-thread's stack range. TIB is per-fiber (our MASM swaps
        // StackBase/StackLimit on every jump_fcontext), so GetCurrentThreadStackLimits
        // returns the active fiber's bounds.
        void CaptureCurrentStackBounds()
        {
            #ifdef _WIN32
            ULONG_PTR low = 0, high = 0;
            ::GetCurrentThreadStackLimits(&low, &high);
            StackBottom = reinterpret_cast<void*>(low);
            StackSize = static_cast<size_t>(high - low);
            #endif
        }
    };
}
