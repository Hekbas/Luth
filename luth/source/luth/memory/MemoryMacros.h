#pragma once

#include "luth/memory/MemoryTracker.h"
#include "luth/core/diagnostics/Profiler.h"

// ===================================================================================
// Tracked allocation macros — opt-in, per call-site category tagging
// ===================================================================================
// Usage:
//   auto* ctx = LH_NEW(Memory::Category::Rendering, VulkanContext);
//   LH_DELETE(Memory::Category::Rendering, ctx);

// Single object new/delete
#define LH_NEW(Category, Type, ...)                                                     \
    [&]() -> Type* {                                                                    \
        Type* _lh_ptr = new Type(__VA_ARGS__);                                          \
        ::Luth::Memory::MemoryTracker::RecordAlloc(Category, sizeof(Type));             \
        LH_PROFILE_ALLOC(_lh_ptr, sizeof(Type));                                        \
        return _lh_ptr;                                                                 \
    }()

#define LH_DELETE(Category, ptr)                                                        \
    do {                                                                                \
        if (ptr) {                                                                      \
            ::Luth::Memory::MemoryTracker::RecordFree(Category, sizeof(*(ptr)));        \
            LH_PROFILE_FREE(ptr);                                                       \
            delete (ptr);                                                               \
            (ptr) = nullptr;                                                            \
        }                                                                               \
    } while(0)

// Raw byte allocation (operator new / operator delete)
#define LH_ALLOC(Category, size)                                                        \
    [&]() -> void* {                                                                    \
        void* _lh_ptr = ::operator new(size);                                           \
        ::Luth::Memory::MemoryTracker::RecordAlloc(Category, size);                     \
        LH_PROFILE_ALLOC(_lh_ptr, size);                                                \
        return _lh_ptr;                                                                 \
    }()

#define LH_FREE(Category, ptr, size)                                                    \
    do {                                                                                \
        if (ptr) {                                                                      \
            ::Luth::Memory::MemoryTracker::RecordFree(Category, size);                  \
            LH_PROFILE_FREE(ptr);                                                       \
            ::operator delete(ptr);                                                     \
            (ptr) = nullptr;                                                            \
        }                                                                               \
    } while(0)

// Array new/delete
#define LH_NEW_ARRAY(Category, Type, count)                                             \
    [&]() -> Type* {                                                                    \
        Type* _lh_ptr = new Type[count];                                                \
        ::Luth::Memory::MemoryTracker::RecordAlloc(Category, sizeof(Type) * (count));   \
        LH_PROFILE_ALLOC(_lh_ptr, sizeof(Type) * (count));                              \
        return _lh_ptr;                                                                 \
    }()

#define LH_DELETE_ARRAY(Category, ptr, count)                                           \
    do {                                                                                \
        if (ptr) {                                                                      \
            ::Luth::Memory::MemoryTracker::RecordFree(Category, sizeof(*(ptr)) * (count)); \
            LH_PROFILE_FREE(ptr);                                                       \
            delete[] (ptr);                                                             \
            (ptr) = nullptr;                                                            \
        }                                                                               \
    } while(0)
