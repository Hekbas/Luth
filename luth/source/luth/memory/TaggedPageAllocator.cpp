#include "luthpch.h"
#include "luth/memory/TaggedPageAllocator.h"
#include "luth/memory/MemoryTracker.h"
#include "luth/core/diagnostics/Log.h"

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

    TaggedPageAllocator& TaggedPageAllocator::Get()
    {
        // Function-local static — initialized on first call, destroyed at static-shutdown.
        // App pairs Init/Shutdown with MemoryTracker so RecordAlloc/Free observers stay in scope.
        static TaggedPageAllocator s_Instance;
        return s_Instance;
    }

    void TaggedPageAllocator::Init()
    {
        // Lazy page allocation; nothing to do at init.
    }

    void TaggedPageAllocator::Shutdown()
    {
        SpinLockGuard lock(m_Lock);

        for (Page* page : m_FreePages)
        {
            MemoryTracker::RecordFree(Category::FrameTagged, PAGE_SIZE);
            #ifdef _WIN32
            VirtualFree(page->Base, 0, MEM_RELEASE);
            #endif
            delete page;
        }
        m_FreePages.clear();

        for (Page* page : m_UsedPages)
        {
            MemoryTracker::RecordFree(Category::FrameTagged, PAGE_SIZE);
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
        SpinLockGuard lock(m_Lock);

        // Linear scan — pages-per-tag is small (~10-20). Index-based loop because
        // pop_back invalidates iterators under MSVC's _ITERATOR_DEBUG_LEVEL=2.
        for (size_t i = 0; i < m_UsedPages.size(); )
        {
            Page* page = m_UsedPages[i];
            if (page->Tag == tag)
            {
                ReturnPage(page);
                m_UsedPages[i] = m_UsedPages.back();
                m_UsedPages.pop_back();
                // don't increment i; check the swapped element
            }
            else
            {
                ++i;
            }
        }
    }

    TaggedPageAllocator::Page* TaggedPageAllocator::AllocatePage(u32 tag)
    {
        SpinLockGuard lock(m_Lock);

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

            MemoryTracker::RecordAlloc(Category::FrameTagged, PAGE_SIZE);
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
