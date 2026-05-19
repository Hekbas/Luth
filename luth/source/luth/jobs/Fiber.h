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

// ASan fiber-switch annotations. Paired around each Win32/jump_fcontext switch so the
// sanitizer can keep its stack-tracking accurate across fiber boundaries. Compiled out
// when ASan is not in the build.
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

    // Worker scheduler fiber wrapper. Two backends share a public API:
    //   - Custom x86_64 MASM context switch (default) — required for ASan; FiberStack
    //     gives us bottom+size before the first switch, fixing the chicken-and-egg with
    //     Win32 fibers' opaque stacks.
    //   - Win32 fibers (LUTH_USE_WIN32_FIBERS) — legacy path retained for one validation
    //     cycle, deleted in a follow-up.
    // See arch/fiber-system.md for the V1-V6 hazard model.
    struct Fiber
    {
        // ── Common state ──
        void* Args = nullptr;
        Fiber* NextWaiting = nullptr;
        u32 PinnedThreadIndex = ~0u;
        bool IsFinished = false;
        std::atomic<u8> State{0};
        void* WaitCounter = nullptr;
        u32 WaitTarget = 0;

        // ASan tracking: AsanFakeStack is the per-fiber save slot; StackBottom/StackSize
        // are the bounds passed to start_switch_fiber. Both populated by Create or
        // CaptureCurrentThreadAsFiber; inert under non-ASan builds.
        void* AsanFakeStack = nullptr;
        void* StackBottom = nullptr;
        size_t StackSize = 0;

#if defined(LUTH_USE_WIN32_FIBERS)
        void* Handle = nullptr;
#else
        // Custom backend: saved RSP (initial value from make_fcontext, updated by each
        // jump_fcontext). Stack region owned by Stack — null for fibers wrapping an
        // existing OS-thread stack via CaptureCurrentThreadAsFiber.
        void* Context = nullptr;
        FiberStack Stack{};
#endif

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
#if defined(LUTH_USE_WIN32_FIBERS)
            Handle = other.Handle;
            other.Handle = nullptr;
#else
            Context = other.Context;
            Stack = other.Stack;
            other.Context = nullptr;
            other.Stack = FiberStack{};
#endif
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
#if defined(LUTH_USE_WIN32_FIBERS)
                Handle = other.Handle;
                other.Handle = nullptr;
#else
                Context = other.Context;
                Stack = other.Stack;
                other.Context = nullptr;
                other.Stack = FiberStack{};
#endif
            }
            return *this;
        }

        Fiber(const Fiber&) = delete;
        Fiber& operator=(const Fiber&) = delete;

        bool IsPinned() const { return PinnedThreadIndex != ~0u; }

        using EntryPoint = void(*)(void*);

        // 2 MB stack — sized for heavy importers (Assimp). On the custom backend,
        // ownerCtx is patched into the new fiber's TIB ArbitraryUserPointer save slot
        // so its first resume restores the correct JobContext into gs:[0x28].
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

#if defined(LUTH_USE_WIN32_FIBERS)
            #ifdef _WIN32
            f.Handle = ::CreateFiberEx(64 * 1024, stackSize, FIBER_FLAG_FLOAT_SWITCH,
                                        (LPFIBER_START_ROUTINE)entry, args);
            if (!f.Handle)
            {
                LH_CORE_CRITICAL("CreateFiberEx failed; err {0}", ::GetLastError());
            }
            #endif
            (void)ownerCtx; // unused on Win32 backend (FLS handles per-fiber lookup)
#else
            f.Stack = AllocateFiberStack(stackSize);
            if (!f.Stack.UsableBottom)
            {
                LH_CORE_CRITICAL("FiberStack allocation failed; aborting Fiber::Create");
                return f;
            }
            f.StackBottom = f.Stack.UsableBottom;
            f.StackSize = f.Stack.UsableSize;
            f.Context = make_fcontext(f.Stack.StackTop, f.Stack.UsableSize, entry, args);
            // Patch the save area so first jump_fcontext restores ownerCtx to gs:[0x28].
            fcontext_set_owner(f.Context, ownerCtx);
#endif

            return f;
        }

        static void Destroy(Fiber& f)
        {
#if defined(LUTH_USE_WIN32_FIBERS)
            #ifdef _WIN32
            if (f.Handle) ::DeleteFiber(f.Handle);
            #endif
            f.Handle = nullptr;
#else
            FreeFiberStack(f.Stack);
            f.Context = nullptr;
            f.StackBottom = nullptr;
            f.StackSize = 0;
#endif
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

#if defined(LUTH_USE_WIN32_FIBERS)
            #ifdef _WIN32
            ::SwitchToFiber(to.Handle);
            #endif
#else
            jump_fcontext(&from.Context, to.Context);
#endif

            #if defined(__SANITIZE_ADDRESS__)
            __sanitizer_finish_switch_fiber(from.AsanFakeStack, nullptr, nullptr);
            #endif
        }

        // Wrap the calling OS thread's existing stack as a Fiber. The custom backend
        // just records bounds and seeds gs:[0x28] = ownerCtx so subsequent
        // jump_fcontext save cycles capture it. Win32 backend uses ConvertThreadToFiberEx.
        static Fiber CaptureCurrentThreadAsFiber(JobContext* ownerCtx)
        {
            Fiber f;
            f.PinnedThreadIndex = ~0u;
            f.IsFinished = false;
            f.State = 1;
            f.WaitCounter = nullptr;
            f.WaitTarget = 0;
            f.CaptureCurrentStackBounds();

#if defined(LUTH_USE_WIN32_FIBERS)
            #ifdef _WIN32
            if (::IsThreadAFiber())
                f.Handle = ::GetCurrentFiber();
            else
                f.Handle = ::ConvertThreadToFiberEx(nullptr, FIBER_FLAG_FLOAT_SWITCH);
            #endif
            (void)ownerCtx;
#else
            #ifdef _WIN32
            __writegsqword(0x28, reinterpret_cast<uintptr_t>(ownerCtx));
            #endif
#endif

            return f;
        }

        // Capture the current OS-thread's stack range. TIB is per-fiber on both backends
        // (Win32 updates it in SwitchToFiber; our MASM swaps it in jump_fcontext), so
        // GetCurrentThreadStackLimits returns the active fiber's bounds.
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
