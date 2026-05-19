#include "luthpch.h"
#include "luth/jobs/FiberPrimitive.h"

// ASan finish_switch_fiber hook — paired with the start_switch_fiber call on the source
// side of Fiber::SwitchTo. Each fiber's FIRST run needs this hook on its own stack so
// ASan reorients tracking (Folly's pattern; boost::context's asm-side equivalent has a
// known ordering bug, see github.com/boostorg/context/issues/65).
#if defined(__SANITIZE_ADDRESS__)
extern "C" {
    void __sanitizer_finish_switch_fiber(void* fake_stack_save,
                                          const void** bottom_old,
                                          size_t* size_old);
}
#endif

namespace Luth::JobSystem
{
    // Called from fiber_entry_trampoline (FiberPrimitive.asm) on a fresh fiber's first run.
    extern "C" void fiber_entry_helper(void (*entry)(void*), void* args)
    {
        #if defined(__SANITIZE_ADDRESS__)
        __sanitizer_finish_switch_fiber(nullptr, nullptr, nullptr);
        #endif

        entry(args);
    }
}
