#include "luthpch.h"
#include "luth/jobs/FiberStackAllocator.h"
#include "luth/core/diagnostics/Log.h"

#ifdef _WIN32
#include <windows.h>
#endif

namespace Luth::JobSystem
{
    // 16 KB guard at the low end. Single 4 KB page would fault on most overflows but a leaf with a large alloca
    // could jump past it; 4 pages gives the fault detector enough cushion without meaningfully eating into the per-fiber footprint.
    static constexpr size_t kGuardSize = 16 * 1024;

    FiberStack AllocateFiberStack(size_t usableSize)
    {
        FiberStack s{};

        #ifdef _WIN32
        const size_t totalSize = usableSize + kGuardSize;

        void* region = ::VirtualAlloc(nullptr, totalSize,
                                       MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
        if (!region)
        {
            LH_LOG(Jobs, critical, "VirtualAlloc failed for fiber stack ({0} bytes); err {1}",
                             totalSize, ::GetLastError());
            return s;
        }

        DWORD oldProtect = 0;
        if (!::VirtualProtect(region, kGuardSize, PAGE_NOACCESS, &oldProtect))
        {
            LH_LOG(Jobs, critical, "VirtualProtect failed for fiber stack guard; err {0}",
                             ::GetLastError());
            ::VirtualFree(region, 0, MEM_RELEASE);
            return s;
        }

        s.Region       = region;
        s.RegionSize   = totalSize;
        s.UsableBottom = static_cast<u8*>(region) + kGuardSize;
        s.UsableSize   = usableSize;
        s.StackTop     = static_cast<u8*>(region) + totalSize;
        #endif

        return s;
    }

    void FreeFiberStack(FiberStack& stack)
    {
        #ifdef _WIN32
        if (stack.Region)
        {
            ::VirtualFree(stack.Region, 0, MEM_RELEASE);
        }
        #endif
        stack = {};
    }
}
