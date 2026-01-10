#pragma once

#include "luth/core/LuthTypes.h"
#include "luth/core/Log.h"

#include <memory>
#include <vector>

namespace Luth
{
    // =============================================
    //              Memory Utils
    // =============================================
    namespace Memory
    {
        constexpr u32 KB = 1024;
        constexpr u32 MB = 1024 * KB;
        constexpr u32 GB = 1024 * MB;

        inline void* AlignForward(void* address, u8 alignment)
        {
            return (void*)((reinterpret_cast<u64>(address) + static_cast<u64>(alignment - 1)) & static_cast<u64>(~(alignment - 1)));
        }
    }

    // =============================================
    //              Allocator Interface
    // =============================================
    class Allocator
    {
    public:
        virtual ~Allocator() = default;

        virtual void* Allocate(u64 size, u8 alignment = 8) = 0;
        
        template<typename T, typename... Args>
        T* New(Args&&... args)
        {
            void* mem = Allocate(sizeof(T), alignof(T));
            return new(mem) T(std::forward<Args>(args)...);
        }

        virtual void Reset() = 0;
    };

    // =============================================
    //              Linear Allocator
    // =============================================
    // Extremely fast "Bump Pointer" allocator.
    // O(1) allocation. Cannot free individual items.
    // Use for per-frame data.
    class LinearAllocator : public Allocator
    {
    public:
        LinearAllocator(u64 size)
            : m_TotalSize(size)
        {
            m_Start = malloc(size);
            LH_PROFILE_ALLOC(m_Start, size);
            m_Current = m_Start;
        }

        ~LinearAllocator()
        {
            LH_PROFILE_FREE(m_Start);
            free(m_Start);
        }

        void* Allocate(u64 size, u8 alignment = 8) override
        {
            LH_CORE_ASSERT(size > 0, "Allocation size must be > 0");

            void* currentAddress = m_Current;
            void* alignedAddress = Memory::AlignForward(currentAddress, alignment);
            
            u64 adjustment = (u64)alignedAddress - (u64)currentAddress;
            u64 neededSize = size + adjustment;

            u64 usedMemory = (u64)m_Current - (u64)m_Start;
            if (usedMemory + neededSize > m_TotalSize)
            {
                LH_CORE_ASSERT(false, "LinearAllocator overflow!");
                return nullptr;
            }

            m_Current = (void*)((u64)alignedAddress + size);
            return alignedAddress;
        }

        void Reset() override
        {
            m_Current = m_Start;
        }

        u64 GetUsedMemory() const { return (u64)m_Current - (u64)m_Start; }
        u64 GetTotalSize() const { return m_TotalSize; }

    private:
        void* m_Start = nullptr;
        void* m_Current = nullptr;
        u64 m_TotalSize = 0;
    };
}
