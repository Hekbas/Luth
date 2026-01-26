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
            // Calculate adjustment for alignment
            // This is a simplified version. Real version needs proper alignment math.
            
            byte* current = m_Current.load();
            // ... atomic add ...
            // For now, simple non-thread-safe bump for prototype
            // TODO: Implement atomic bump
            
            return nullptr; 
        }

        void Reset()
        {
            m_Current.store(m_Start);
        }

    private:
        byte* m_Start = nullptr;
        size_t m_Size = 0;
        std::atomic<byte*> m_Current;
    };
}
