#pragma once

// Convenience header: includes all memory subsystem headers

#include "luth/core/types/LuthTypes.h"
#include "luth/memory/LinearAllocator.h"
#include "luth/memory/TaggedPageAllocator.h"

namespace Luth::Memory
{
    constexpr u32 KB = 1024;
    constexpr u32 MB = 1024 * KB;
    constexpr u32 GB = 1024 * MB;

    inline void* AlignForward(void* address, u8 alignment)
    {
        return (void*)((reinterpret_cast<u64>(address) + static_cast<u64>(alignment - 1)) & static_cast<u64>(~(alignment - 1)));
    }
}
