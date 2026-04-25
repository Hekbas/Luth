// Global operator new/delete overrides — wires every heap allocation through
// Tracy's memory profiler so STL containers and third-party libs are visible
// in capture-time analysis. Engine-categorized allocations (LH_NEW/LH_ALLOC)
// keep their separate tracking via MemoryTracker; see arch/memory.md.
//
// Active only when TRACY_ENABLE is defined (Debug + Release configs).
// Dist builds use the default new/delete with zero overhead.

#include "luthpch.h"

#if defined(TRACY_ENABLE)

#include <new>
#include <cstdlib>
#include <tracy/Tracy.hpp>

void* operator new(std::size_t count)
{
    void* ptr = std::malloc(count);
    if (!ptr) throw std::bad_alloc{};
    TracyAlloc(ptr, count);
    return ptr;
}

void* operator new[](std::size_t count)
{
    void* ptr = std::malloc(count);
    if (!ptr) throw std::bad_alloc{};
    TracyAlloc(ptr, count);
    return ptr;
}

void* operator new(std::size_t count, const std::nothrow_t&) noexcept
{
    void* ptr = std::malloc(count);
    if (ptr) TracyAlloc(ptr, count);
    return ptr;
}

void* operator new[](std::size_t count, const std::nothrow_t&) noexcept
{
    void* ptr = std::malloc(count);
    if (ptr) TracyAlloc(ptr, count);
    return ptr;
}

void operator delete(void* ptr) noexcept
{
    TracyFree(ptr);
    std::free(ptr);
}

void operator delete[](void* ptr) noexcept
{
    TracyFree(ptr);
    std::free(ptr);
}

void operator delete(void* ptr, std::size_t) noexcept
{
    TracyFree(ptr);
    std::free(ptr);
}

void operator delete[](void* ptr, std::size_t) noexcept
{
    TracyFree(ptr);
    std::free(ptr);
}

void operator delete(void* ptr, const std::nothrow_t&) noexcept
{
    TracyFree(ptr);
    std::free(ptr);
}

void operator delete[](void* ptr, const std::nothrow_t&) noexcept
{
    TracyFree(ptr);
    std::free(ptr);
}

#endif // TRACY_ENABLE
