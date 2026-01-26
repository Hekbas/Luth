#pragma once

#include "luth/core/LuthTypes.h"
#include <atomic>

namespace Luth::Memory
{
    class LinearAllocator
    {
    public:
        LinearAllocator(size_t size)
            : m_Size(size)
        {
            m_Start = new byte[size];
            m_Current.store(m_Start);
        }

        ~LinearAllocator()
        {
            delete[] m_Start;
        }

        void* Allocate(size_t size, size_t alignment = 8)
        {
            byte* currentPtr = m_Current.load(std::memory_order_relaxed);
            byte* nextPtr = nullptr;
            
            // Calculate aligned address
            // uintptr_t currentAddr = reinterpret_cast<uintptr_t>(currentPtr);
            // uintptr_t alignedAddr = (currentAddr + alignment - 1) & ~(alignment - 1);
            // byte* alignedPtr = reinterpret_cast<byte*>(alignedAddr);
            
            // Simple bump for now (assuming 8-byte alignment is sufficient for most things or pre-aligned)
            // TODO: Proper alignment logic
            
            do {
                // Check if we have enough space
                if (currentPtr + size > m_Start + m_Size)
                {
                    return nullptr; // Out of memory
                }
                nextPtr = currentPtr + size;
            } while (!m_Current.compare_exchange_weak(currentPtr, nextPtr, std::memory_order_acquire, std::memory_order_relaxed));

            return currentPtr;
        }

        void Reset()
        {
            m_Current.store(m_Start, std::memory_order_release);
        }

    private:
        byte* m_Start = nullptr;
        size_t m_Size = 0;
        std::atomic<byte*> m_Current;
    };
}
