#include "luthpch.h"
#include "luth/memory/TaggedPageAllocator.h"
#include "luth/core/Log.h"

#ifdef _WIN32
#include <windows.h>
#endif

namespace Luth::Memory
{
    TaggedPageAllocator::TaggedPageAllocator()
    {
    }

    TaggedPageAllocator::~TaggedPageAllocator()
    {
        Shutdown();
    }

    void TaggedPageAllocator::Init()
    {
        // Pre-allocate some pages?
        // For now, lazy allocation is fine.
    }

    void TaggedPageAllocator::Shutdown()
    {
        std::lock_guard<std::mutex> lock(m_Lock);

        // Free all pages
        for (Page* page : m_FreePages)
        {
            #ifdef _WIN32
            VirtualFree(page->Base, 0, MEM_RELEASE);
            #endif
            delete page;
        }
        m_FreePages.clear();

        for (Page* page : m_UsedPages)
        {
            #ifdef _WIN32
            VirtualFree(page->Base, 0, MEM_RELEASE);
            #endif
            delete page;
        }
        m_UsedPages.clear();
    }

    void* TaggedPageAllocator::Allocate(ThreadCache& cache, u64 size, u64 alignment)
    {
        // 1. Try to allocate from the active page in the cache
        Page* page = cache.ActivePage;

        if (page)
        {
            void* currentAddress = (void*)((u64)page->Base + page->Used);
            void* alignedAddress = (void*)((reinterpret_cast<u64>(currentAddress) + (alignment - 1)) & ~(alignment - 1));
            
            u64 adjustment = (u64)alignedAddress - (u64)currentAddress;
            u64 neededSize = size + adjustment;

            if (page->Used + neededSize <= PAGE_SIZE)
            {
                page->Used += neededSize;
                return alignedAddress;
            }
        }

        // 2. Page full or no page. Get a new one.
        // If we had a page, it remains in the used list (managed by the allocator), 
        // we just stop writing to it.
        
        // Note: The old page is already in m_UsedPages (added when allocated).
        // We just need a new one.
        
        // IMPORTANT: The tag comes from the cache. The user sets the tag on the cache before allocating.
        // Wait, the API says Allocate(cache, size). Where is the tag set?
        // The cache has 'CurrentTag'.
        
        Page* newPage = AllocatePage(cache.CurrentTag);
        if (!newPage) return nullptr;

        cache.ActivePage = newPage;
        
        // Retry allocation on new page
        // New page starts at offset 0
        void* alignedAddress = (void*)((reinterpret_cast<u64>(newPage->Base) + (alignment - 1)) & ~(alignment - 1));
        u64 adjustment = (u64)alignedAddress - (u64)newPage->Base;
        newPage->Used = size + adjustment;
        
        return alignedAddress;
    }

    void TaggedPageAllocator::FreeTag(u32 tag)
    {
        std::lock_guard<std::mutex> lock(m_Lock);

        // Move pages with matching tag from Used to Free
        // We iterate and remove.
        
        auto it = m_UsedPages.begin();
        while (it != m_UsedPages.end())
        {
            Page* page = *it;
            if (page->Tag == tag)
            {
                ReturnPage(page);
                // Swap with last for O(1) removal
                *it = m_UsedPages.back();
                m_UsedPages.pop_back();
                // Don't increment iterator, check the swapped element
            }
            else
            {
                ++it;
            }
        }
    }

    TaggedPageAllocator::Page* TaggedPageAllocator::AllocatePage(u32 tag)
    {
        std::lock_guard<std::mutex> lock(m_Lock);

        Page* page = nullptr;

        if (!m_FreePages.empty())
        {
            page = m_FreePages.back();
            m_FreePages.pop_back();
        }
        else
        {
            // Allocate new from OS
            page = new Page();
            #ifdef _WIN32
            page->Base = VirtualAlloc(nullptr, PAGE_SIZE, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
            #endif
            
            if (!page->Base)
            {
                LH_CORE_CRITICAL("TaggedPageAllocator: VirtualAlloc failed!");
                delete page;
                return nullptr;
            }
        }

        page->Used = 0;
        page->Tag = tag;
        page->Next = nullptr;
        
        m_UsedPages.push_back(page);
        return page;
    }

    void TaggedPageAllocator::ReturnPage(Page* page)
    {
        // Assumes lock is held
        page->Used = 0;
        page->Tag = 0; // Clear tag
        m_FreePages.push_back(page);
    }
}
