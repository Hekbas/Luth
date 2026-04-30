#pragma once

#include "luth/core/types/LuthTypes.h"
#include "luth/core/diagnostics/Log.h"
#include <atomic>
#include <cassert>
#include <cstdint>

#ifdef _WIN32
#include <windows.h>
#else
#include <ucontext.h>
#include <sys/mman.h>
#include <cstdlib>
#include <cstring>
#include <cerrno>
#include <unistd.h>
#endif

namespace Luth::JobSystem
{
    // Forward declare — implemented in JobSystem.cpp via FLS
    struct JobContext;
    JobContext* GetCurrentJobContext();

#ifndef _WIN32
    struct LinuxFiberData 
    {
        ucontext_t context{};
        void* stack = nullptr;
        u32 stackSize = 0;
        void(*entry)(void*) = nullptr;
        void* args = nullptr;
    };

    inline thread_local LinuxFiberData* t_CurrentFiberData = nullptr;

    extern "C" void FiberTrampoline(uint32_t low, uint32_t high) 
    {
        uintptr_t ptr = ((uintptr_t)low) | (((uintptr_t)high) << 32);
        LinuxFiberData* ctx = (LinuxFiberData*)ptr;
        ctx->entry(ctx->args);
        std::abort(); // Fibers should never return; if they do, something went wrong.
    }

    static void* AllocateStack(u32 size)
    {
        const size_t pageSize = sysconf(_SC_PAGESIZE);
        size = (size + pageSize - 1) & ~(pageSize - 1);

        void* mem = mmap(nullptr, size + pageSize,
                        PROT_READ | PROT_WRITE,
                        MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);

        if (mem == MAP_FAILED)
            return nullptr;

        mprotect(mem, pageSize, PROT_NONE);
        return static_cast<char*>(mem) + pageSize;
    }

    static void FreeStack(void* stack, u32 size)
    {
        if (!stack) return;

        const size_t pageSize = sysconf(_SC_PAGESIZE);
        void* base = static_cast<char*>(stack) - pageSize;

        munmap(base, size + pageSize);
    }
#endif

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
            #else
            auto* data = new LinuxFiberData{};
            data->entry = entry;
            data->args = args;
            data->stackSize = stackSize;

            data->stack = AllocateStack(stackSize);
            if (!data->stack)
            {
                LH_CORE_CRITICAL("Failed to allocate fiber stack! Error: {0}",strerror(errno));
                delete data;
                return f;
            }

            getcontext(&data->context);
            data->context.uc_stack.ss_sp = data->stack;
            data->context.uc_stack.ss_size = stackSize;
            data->context.uc_link = nullptr;

            uintptr_t ptr = reinterpret_cast<uintptr_t>(data);
            uint32_t low = (uint32_t)(ptr & 0xFFFFFFFF);
            uint32_t high = (uint32_t)(ptr >> 32);
            makecontext(&data->context, (void(*)())FiberTrampoline, 2, low, high);

            f.Handle = data;
            #endif

            return f;
        }

        static void Destroy(Fiber& f)
        {
            #ifdef _WIN32
            if (f.Handle) DeleteFiber(f.Handle);
            #else
            if (f.Handle)
            {
                auto* data = static_cast<LinuxFiberData*>(f.Handle);
                if (data->stack && data->stackSize > 0)
                    FreeStack(data->stack, data->stackSize);
                delete data;
            }
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
            #else
            auto* fromData = t_CurrentFiberData;
            auto* toData = static_cast<LinuxFiberData*>(f.Handle);
            assert(toData && "Attempting to switch to an uninitialized fiber!");
            t_CurrentFiberData = toData;
            swapcontext(&fromData->context, &toData->context);
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
            #else
            static thread_local LinuxFiberData t_ThreadData;
            static thread_local bool t_ThreadDataInitialized = false;
            if (!t_ThreadDataInitialized)
            {
                getcontext(&t_ThreadData.context);
                t_ThreadData.stack = nullptr;
                t_ThreadData.stackSize = 0;
                t_ThreadData.entry = nullptr;
                t_ThreadData.args = args;

                t_CurrentFiberData = &t_ThreadData;
                t_ThreadDataInitialized = true;
            }

            f.Handle = &t_ThreadData;
            #endif
            return f;
        }
    };
}