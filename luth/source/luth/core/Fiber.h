#pragma once

#include "luth/core/LuthTypes.h"

#ifdef _WIN32
#include <windows.h>
#endif

namespace Luth::JobSystem
{
    struct Fiber
    {
        void* Handle = nullptr;
        void* Args = nullptr; // User data passed to the fiber function
        
        // Function pointer for the fiber entry point
        using EntryPoint = void(*)(void*);
        
        static Fiber Create(EntryPoint entry, void* args, u32 stackSize = 64 * 1024)
        {
            Fiber f;
            #ifdef _WIN32
            f.Handle = CreateFiber(stackSize, (LPFIBER_START_ROUTINE)entry, args);
            #endif
            f.Args = args;
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
            f.Handle = ::ConvertThreadToFiber(args);
            #endif
            f.Args = args;
            return f;
        }
    };
}
