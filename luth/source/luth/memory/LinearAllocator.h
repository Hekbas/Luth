#pragma once

#include "luth/core/LuthTypes.h"
#include "luth/memory/MemoryTracker.h"
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
            m_PageSizes.push_back(m_DefaultPageSize);
            MemoryTracker::RecordAlloc(Category::FrameLinear, m_DefaultPageSize);
            m_CurrentPage = 0;
            m_CurrentPtr = m_Pages[0];
        }

        ~LinearAllocator()
        {
            for (size_t i = 0; i < m_Pages.size(); ++i)
            {
                MemoryTracker::RecordFree(Category::FrameLinear, m_PageSizes[i]);
                delete[] m_Pages[i];
            }
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
                    if (size > m_DefaultPageSize)
                    {
                        size_t newSize = std::max(m_DefaultPageSize, size + alignment);
                        m_Pages.push_back(new byte[newSize]);
                        m_PageSizes.push_back(newSize);
                        MemoryTracker::RecordAlloc(Category::FrameLinear, newSize);
                    }
                    else
                    {
                        m_Pages.push_back(new byte[m_DefaultPageSize]);
                        m_PageSizes.push_back(m_DefaultPageSize);
                        MemoryTracker::RecordAlloc(Category::FrameLinear, m_DefaultPageSize);
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

        u64 GetUsedMemory() const
        {
            u64 used = static_cast<u64>(m_CurrentPage) * m_DefaultPageSize;
            if (!m_Pages.empty())
                used += static_cast<u64>(m_CurrentPtr - m_Pages[m_CurrentPage]);
            return used;
        }

        u64 GetTotalSize() const
        {
            return static_cast<u64>(m_Pages.size()) * m_DefaultPageSize;
        }

    private:
        std::vector<byte*> m_Pages;
        std::vector<size_t> m_PageSizes;  // Actual size per page (for accurate free tracking)
        size_t m_DefaultPageSize;
        
        // Non-atomic for now (Thread-Local usage)
        u32 m_CurrentPage = 0;
        byte* m_CurrentPtr = nullptr;
    };
}
