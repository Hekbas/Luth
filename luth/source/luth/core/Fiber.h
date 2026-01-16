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
        
        // Intrusive Linked List for Waiting (Lock-Free Stack Node)
        // Used when this fiber is waiting on an AtomicCounter
        Fiber* NextWaiting = nullptr;

        // Function pointer for the fiber entry point
        using EntryPoint = void(*)(void*);
        
        static Fiber Create(EntryPoint entry, void* args, u32 stackSize = 512 * 1024)
        {
            Fiber f;
            f.Args = args;
            f.NextWaiting = nullptr;

            #ifdef _WIN32
            // Commit 64KB initially, Reserve full size.
            // CreateFiberEx(dwStackCommitSize, dwStackReserveSize, dwFlags, lpStartAddress, lpParameter)
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
