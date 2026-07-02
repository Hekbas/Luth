#pragma once

#include "luth/memory/MemoryTracker.h"
#include "luth/core/diagnostics/Profiler.h"

// Tracked allocation macros: opt-in, per call-site category tagging.
// Usage:
//   auto* ctx = LH_NEW(Memory::Category::Rendering, VulkanContext);
//   LH_DELETE(Memory::Category::Rendering, ctx);
// See docs/development/arch/memory.md for the full policy.
//
// Tracy memory profiling: the global operator new/delete overrides in
// GlobalNewDelete.cpp call TracyAlloc/TracyFree, so the underlying `new`/`delete`
// here is already visible in Tracy's Memory tab. These macros only add the
// engine-side MemoryTracker category counter on top.

// Single object new/delete
#define LH_NEW(Category, Type, ...)                                                     \
    [&]() -> Type* {                                                                    \
        Type* _lh_ptr = new Type(__VA_ARGS__);                                          \
        ::Luth::Memory::MemoryTracker::RecordAlloc(Category, sizeof(Type));             \
        return _lh_ptr;                                                                 \
    }()

#define LH_DELETE(Category, ptr)                                                        \
    do {                                                                                \
        if (ptr) {                                                                      \
            ::Luth::Memory::MemoryTracker::RecordFree(Category, sizeof(*(ptr)));        \
            delete (ptr);                                                               \
            (ptr) = nullptr;                                                            \
        }                                                                               \
    } while(0)

// Raw byte allocation (operator new / operator delete)
#define LH_ALLOC(Category, size)                                                        \
    [&]() -> void* {                                                                    \
        void* _lh_ptr = ::operator new(size);                                           \
        ::Luth::Memory::MemoryTracker::RecordAlloc(Category, size);                     \
        return _lh_ptr;                                                                 \
    }()

#define LH_FREE(Category, ptr, size)                                                    \
    do {                                                                                \
        if (ptr) {                                                                      \
            ::Luth::Memory::MemoryTracker::RecordFree(Category, size);                  \
            ::operator delete(ptr);                                                     \
            (ptr) = nullptr;                                                            \
        }                                                                               \
    } while(0)

// Array new/delete
#define LH_NEW_ARRAY(Category, Type, count)                                             \
    [&]() -> Type* {                                                                    \
        Type* _lh_ptr = new Type[count];                                                \
        ::Luth::Memory::MemoryTracker::RecordAlloc(Category, sizeof(Type) * (count));   \
        return _lh_ptr;                                                                 \
    }()

#define LH_DELETE_ARRAY(Category, ptr, count)                                           \
    do {                                                                                \
        if (ptr) {                                                                      \
            ::Luth::Memory::MemoryTracker::RecordFree(Category, sizeof(*(ptr)) * (count)); \
            delete[] (ptr);                                                             \
            (ptr) = nullptr;                                                            \
        }                                                                               \
    } while(0)
