#pragma once

#include "luth/core/LuthTypes.h"
#include "luth/core/Log.h"

#ifdef _WIN32
#include <windows.h>
#endif

namespace Luth::JobSystem
{
    struct Fiber
    {
        void* Handle = nullptr;
        void* Args = nullptr; // User data passed to the fiber function
        void* StackBase = nullptr; // Pointer to the allocated stack memory (for manual guard pages)
        u32 StackSize = 0;

        // Function pointer for the fiber entry point
        using EntryPoint = void(*)(void*);
        
        static Fiber Create(EntryPoint entry, void* args, u32 stackSize = 512 * 1024)
        {
            Fiber f;
            f.Args = args;
            f.StackSize = stackSize;

            #ifdef _WIN32
            // Windows CreateFiber automatically allocates a stack.
            // However, to strictly enforce guard pages, we might want to use CreateFiberEx 
            // or rely on Windows default guard page behavior (which is usually present).
            // For this implementation, we will stick to CreateFiber but add a commit flag if needed.
            // Note: Windows stacks grow downwards. The system automatically places a guard page.
            // But for custom allocators or stricter control, we would VirtualAlloc manually.
            
            // To be safe and explicit according to the plan, we should verify stack behavior.
            // But CreateFiber does this well on Windows.
            // Let's use CreateFiberEx to be explicit about the commit size vs reserve size.
            
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
            #ifdef _WIN32
            // Check if already a fiber to avoid errors
            if (IsThreadAFiber())
            {
                 f.Handle = GetCurrentFiber();
            }
            else
            {
                f.Handle = ::ConvertThreadToFiber(args);
            }
            #endif
            f.Args = args;
            return f;
        }
    };
}
