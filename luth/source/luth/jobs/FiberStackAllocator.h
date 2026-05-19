#pragma once

#include "luth/core/types/LuthTypes.h"

#include <cstddef>

namespace Luth::JobSystem
{
    // Stack region backing a custom fiber. The guard page sits at the low end so a stack
    // overflow (stack grows down on x86_64) faults instead of corrupting an adjacent
    // allocation. UsableBottom is the lowest address a frame can validly touch;
    // StackTop is the high address used as the initial RSP target by make_fcontext.
    struct FiberStack
    {
        void*  Region        = nullptr; // VirtualAlloc'd base (low address)
        size_t RegionSize    = 0;       // total allocation including guard
        void*  UsableBottom  = nullptr; // first byte above the guard page
        size_t UsableSize    = 0;       // usable stack range = RegionSize - guardSize
        void*  StackTop      = nullptr; // high address; initial RSP
    };

    // Allocate `usableSize` bytes of stack plus a low-end guard page via VirtualAlloc.
    // Returns a fully-populated FiberStack on success; a zero-initialised one on failure
    // (logs LH_CORE_CRITICAL).
    FiberStack AllocateFiberStack(size_t usableSize);

    // Release a stack previously returned by AllocateFiberStack. No-op on a default-
    // constructed FiberStack. Resets `stack` to zero-init.
    void FreeFiberStack(FiberStack& stack);
}
