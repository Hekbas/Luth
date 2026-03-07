#pragma once

#include "luth/core/LuthTypes.h"
#include <atomic>
#include <vector>

namespace Luth::Memory
{
    class LinearAllocator
    {
    public:
        LinearAllocator(size_t size)
            : m_DefaultPageSize(size)
        {
            // Allocate first page
            m_Pages.push_back(new byte[m_DefaultPageSize]);
            m_CurrentPage = 0;
            m_CurrentPtr = m_Pages[0];
        }

        ~LinearAllocator()
        {
            for (byte* page : m_Pages)
                delete[] page;
        }

        template<typename T, typename... Args>
        T* New(Args&&... args)
        {
            void* mem = Allocate(sizeof(T), alignof(T));
            return new(mem) T(std::forward<Args>(args)...);
        }

        void* Allocate(size_t size, size_t alignment = 8)
        {
            // Align current pointer
            uintptr_t currentAddr = reinterpret_cast<uintptr_t>(m_CurrentPtr);
            uintptr_t alignedAddr = (currentAddr + alignment - 1) & ~(alignment - 1);
            byte* alignedPtr = reinterpret_cast<byte*>(alignedAddr);
            
            // Check if fits in current page
            byte* pageStart = m_Pages[m_CurrentPage];
            if (alignedPtr + size > pageStart + m_DefaultPageSize)
            {
                // Move to next page
                m_CurrentPage++;
                
                // Allocate new page if needed
                if (m_CurrentPage >= m_Pages.size())
                {
                    // Handle large allocations? For now assume size < PageSize
                    if (size > m_DefaultPageSize)
                    {
                        // Special case: Large allocation
                        // We could allocate a dedicated large page, but for now let's just assert/fail or alloc bigger
                        // Let's alloc a page big enough
                        size_t newSize = std::max(m_DefaultPageSize, size + alignment);
                        m_Pages.push_back(new byte[newSize]);
                        // Note: This breaks the "DefaultPageSize" assumption for Reset logic if we don't track size
                        // Simplified: Just alloc default size and crash if too big
                    }
                    else
                    {
                        m_Pages.push_back(new byte[m_DefaultPageSize]);
                    }
                }
                
                m_CurrentPtr = m_Pages[m_CurrentPage];
                alignedPtr = m_CurrentPtr; // Re-align (it's start of page, so aligned)
            }
            
            m_CurrentPtr = alignedPtr + size;
            return alignedPtr;
        }

        void Reset()
        {
            m_CurrentPage = 0;
            if (!m_Pages.empty())
                m_CurrentPtr = m_Pages[0];
        }

    private:
        std::vector<byte*> m_Pages;
        size_t m_DefaultPageSize;
        
        // Non-atomic for now (Thread-Local usage)
        u32 m_CurrentPage = 0;
        byte* m_CurrentPtr = nullptr;
    };
}
