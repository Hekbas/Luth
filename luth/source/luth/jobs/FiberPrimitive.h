#pragma once

#include "luth/core/types/LuthTypes.h"

#include <cstddef>

namespace Luth::JobSystem
{
    // Low-level x86_64 Windows context-switch primitives. Replaces the Win32 CreateFiberEx +
    // SwitchToFiber pair. See arch/fiber-system.md.
    //
    // ABI: Win64 MSVC. All callee-saved GPRs + XMM6-XMM15 + MXCSR + 5 NT_TIB fields (StackBase,
    // StackLimit, DeallocationStack, FiberData, ArbitraryUserPointer) are swapped per call to jump_fcontext.

    using FiberEntryFn = void(*)(void*);

    extern "C"
    {
        // Lay down an initial save area at the high end of a freshly-allocated stack so that the first
        // jump_fcontext to the returned context pointer causes `entry(args)` to start running on that
        // stack. `stack_top` is the high address of the stack region (one past the last usable byte);
        // `usable_size` is the byte count of the region (used to populate the TIB StackBase/StackLimit
        // slots in the save area).
        //
        // Returns the initial fiber context pointer (to be passed as the `to_sp` of the first jump_fcontext).
        void* make_fcontext(void* stack_top, size_t usable_size,
                             FiberEntryFn entry, void* args);

        // Save the caller's context to (*out_from_sp), load the destination context from `to_sp`, and resume
        // execution at the destination's saved RIP. Returns when some other fiber later switches back to the caller.
        void jump_fcontext(void** out_from_sp, void* to_sp);
    }

    // Offset of the ArbitraryUserPointer (TIB+0x28) save slot inside the save area laid down by
    // make_fcontext. Keep in sync with FiberPrimitive.asm.
    inline constexpr size_t kFcontextArbitraryUserPointerSlot = 264;

    // Patch a fresh context's save area so the first jump_fcontext to it restores `owner` into
    // gs:[0x28]. Call after make_fcontext, before the first switch.
    inline void fcontext_set_owner(void* context, void* owner)
    {
        auto* p = reinterpret_cast<void**>(
            static_cast<u8*>(context) + kFcontextArbitraryUserPointerSlot);
        *p = owner;
    }
}
