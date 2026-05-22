#include "luthpch.h"
#include "luth/memory/GPUTaggedPageAllocator.h"
#include "luth/memory/MemoryMacros.h"
#include "luth/memory/MemoryTracker.h"
#include "luth/core/diagnostics/Log.h"
#include "luth/renderer/backend/vulkan/VulkanAllocator.h"
#include "luth/renderer/backend/vulkan/VulkanContext.h"

#include <vma/vk_mem_alloc.h>

namespace Luth::Memory
{
    GPUTaggedPageAllocator::GPUTaggedPageAllocator() = default;
    GPUTaggedPageAllocator::~GPUTaggedPageAllocator() = default;

    GPUTaggedPageAllocator& GPUTaggedPageAllocator::Get()
    {
        static GPUTaggedPageAllocator s_Instance;
        return s_Instance;
    }

    void GPUTaggedPageAllocator::Init()
    {
        // Storage-buffer offset alignment is the minimum any descriptor binding may use;
        // raise the alignment floor so AllocateMappedSequentialBuffer-backed regions can
        // bind directly without further fixup at consumer sites.
        const auto& props = VulkanContext::Get().GetPhysicalDeviceProperties();
        m_MinAlignment = std::max<u64>(props.limits.minStorageBufferOffsetAlignment, 16);

        // Pre-allocate one backing buffer so the first hot-path allocation doesn't fault into VMA.
        SpinLockGuard lock(m_Lock);
        GrowBackingPoolLocked();
    }

    void GPUTaggedPageAllocator::Shutdown()
    {
        SpinLockGuard lock(m_Lock);

        // Device is idle by this point (VulkanBackend::Shutdown calls vkDeviceWaitIdle first),
        // so immediate destroy is safe — no need to round-trip through the deletion queue.
        for (GPUPage* page : m_UsedPages)
        {
            if (page->isLargeOneShot && page->oneShotAlloc)
                VulkanAllocator::FreeBuffer(page->buffer, page->oneShotAlloc);
            LH_DELETE(Memory::Category::GPU, page);
        }
        m_UsedPages.clear();

        for (GPUPage* page : m_FreePages)
            LH_DELETE(Memory::Category::GPU, page);
        m_FreePages.clear();

        for (BackingBuffer& bb : m_BackingBuffers)
        {
            if (bb.buffer)
                VulkanAllocator::FreeBuffer(bb.buffer, bb.alloc);
        }
        m_BackingBuffers.clear();
    }

    GPUSubRegion GPUTaggedPageAllocator::Allocate(GPUThreadCache& cache, u64 size, u64 alignment)
    {
        if (size > PAGE_SIZE)
            return AllocateLargeTagged(cache.CurrentTag, size, alignment);

        const u64 align = std::max(alignment, m_MinAlignment);

        // Hot path — bump within active page (no lock; cache is per-fiber).
        // Tag-mismatch invalidates the cached page: a fiber reused across frames may
        // see ActivePage tagged with a prior frame; bumping into it would defeat
        // FreeTag (the page would never reach matching-tag bulk-release for either frame).
        if (cache.ActivePage && cache.ActivePage->tag != cache.CurrentTag)
            cache.ActivePage = nullptr;

        if (cache.ActivePage)
        {
            const u64 cur     = cache.ActivePage->baseOffset + cache.ActivePage->used;
            const u64 aligned = (cur + align - 1) & ~(align - 1);
            const u64 endIfFit = aligned + size;
            const u64 pageEnd  = cache.ActivePage->baseOffset + PAGE_SIZE;
            if (endIfFit <= pageEnd)
            {
                cache.ActivePage->used = endIfFit - cache.ActivePage->baseOffset;
                GPUSubRegion r;
                r.buffer    = cache.ActivePage->buffer;
                r.offset    = aligned;
                r.size      = size;
                r.mappedPtr = static_cast<u8*>(cache.ActivePage->basePtr) + (aligned - cache.ActivePage->baseOffset);
                return r;
            }
        }

        // Overflow → claim a new page, then retry on it (guaranteed to fit since size <= PAGE_SIZE).
        {
            SpinLockGuard lock(m_Lock);
            cache.ActivePage = AllocatePageLocked(cache.CurrentTag);
        }

        if (!cache.ActivePage)
        {
            LH_CORE_CRITICAL("GPUTaggedPageAllocator: AllocatePage failed!");
            return {};
        }

        const u64 base    = cache.ActivePage->baseOffset;
        const u64 aligned = (base + align - 1) & ~(align - 1);
        cache.ActivePage->used = aligned + size - base;
        GPUSubRegion r;
        r.buffer    = cache.ActivePage->buffer;
        r.offset    = aligned;
        r.size      = size;
        r.mappedPtr = static_cast<u8*>(cache.ActivePage->basePtr) + (aligned - base);
        return r;
    }

    GPUSubRegion GPUTaggedPageAllocator::AllocateLargeTagged(u32 tag, u64 size, u64 alignment)
    {
        // Dedicated VkBuffer per request; tagged like a page so FreeTag releases it.
        // Backing alignment is whatever VMA picks; we don't apply m_MinAlignment to offset
        // because offset = 0 for one-shot allocations (the whole buffer is one region).
        (void)alignment;

        VkBufferCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        info.size  = size;
        info.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT
                   | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT
                   | VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT
                   | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT
                   | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
        // Universal CPU→GPU data path — Material / Bone / Object / Indirect / per-frame UBOs all sub-allocate from
        // these large-one-shot allocations. Async-compute passes may read SSBOs allocated here in a future effort
        // (forward-plus cluster build). CONCURRENT carries no overhead on NVIDIA per Khronos guidance; AMD DCC
        // concerns are color-RT specific and don't apply to buffers.
        VulkanContext::Get().ApplyConcurrentSharing(info);

        VkBuffer buf  = VK_NULL_HANDLE;
        void*    map  = nullptr;
        VmaAllocation a = VulkanAllocator::AllocateMappedSequentialBuffer(info, buf, &map);
        if (!a)
        {
            LH_CORE_CRITICAL("GPUTaggedPageAllocator::AllocateLargeTagged failed for size {0}", size);
            return {};
        }

        GPUPage* page = LH_NEW(Memory::Category::GPU, GPUPage);
        page->isLargeOneShot     = true;
        page->oneShotAlloc       = a;
        page->buffer             = buf;
        page->basePtr            = map;
        page->baseOffset         = 0;
        page->used               = size;
        page->tag                = tag;

        {
            SpinLockGuard lock(m_Lock);
            m_UsedPages.push_back(page);
        }

        GPUSubRegion r;
        r.buffer    = buf;
        r.offset    = 0;
        r.size      = size;
        r.mappedPtr = map;
        return r;
    }

    void GPUTaggedPageAllocator::FlushRegion(const GPUSubRegion& region)
    {
        if (!region.buffer || region.size == 0) return;

        // Map region.buffer back to its VmaAllocation. Backings carry their alloc;
        // large-one-shot pages carry it on the page. Linear search across backings is
        // cheap (typically 1-2 entries).
        SpinLockGuard lock(m_Lock);
        for (const BackingBuffer& bb : m_BackingBuffers)
        {
            if (bb.buffer == region.buffer)
            {
                VulkanAllocator::FlushSlice(bb.alloc, region.offset, region.size);
                return;
            }
        }
        for (const GPUPage* page : m_UsedPages)
        {
            if (page->isLargeOneShot && page->buffer == region.buffer)
            {
                VulkanAllocator::FlushSlice(page->oneShotAlloc, region.offset, region.size);
                return;
            }
        }
    }

    void GPUTaggedPageAllocator::FreeTag(u32 tag)
    {
        SpinLockGuard lock(m_Lock);

        // Linear scan — small N (~10-20 pages per frame). Index-based loop because
        // pop_back invalidates iterators under MSVC's _ITERATOR_DEBUG_LEVEL=2; the
        // cached end() trips the iterator-compatibility check on the next compare.
        for (size_t i = 0; i < m_UsedPages.size(); )
        {
            GPUPage* page = m_UsedPages[i];
            if (page->tag == tag)
            {
                if (page->isLargeOneShot)
                {
                    VulkanAllocator::FreeBuffer(page->buffer, page->oneShotAlloc);
                    LH_DELETE(Memory::Category::GPU, page);
                }
                else
                {
                    page->used = 0;
                    page->tag  = 0;
                    m_FreePages.push_back(page);
                }
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

    GPUTaggedPageAllocator::Stats GPUTaggedPageAllocator::GetStats() const
    {
        // Best-effort lock-free read. Counts are coarse-stable; ProfilerPanel UI is fine with that.
        Stats s;
        s.BackingBuffers = static_cast<u32>(m_BackingBuffers.size());
        s.FreePages      = static_cast<u32>(m_FreePages.size());
        u64 inFlight = 0;
        u32 active = 0, oneShots = 0;
        for (const GPUPage* page : m_UsedPages)
        {
            if (page->isLargeOneShot) { ++oneShots; inFlight += page->used; }
            else                      { ++active;   inFlight += page->used; }
        }
        s.ActivePages   = active;
        s.LargeOneShots = oneShots;
        s.BytesInFlight = inFlight;
        return s;
    }

    GPUPage* GPUTaggedPageAllocator::AllocatePageLocked(u32 tag)
    {
        // Free pool first.
        if (!m_FreePages.empty())
        {
            GPUPage* page = m_FreePages.back();
            m_FreePages.pop_back();
            page->tag  = tag;
            page->used = 0;
            m_UsedPages.push_back(page);
            return page;
        }

        // Try to carve a fresh page from any backing with room.
        for (u32 i = 0; i < m_BackingBuffers.size(); ++i)
        {
            BackingBuffer& bb = m_BackingBuffers[i];
            if (bb.pagesUsed < BACKING_SIZE / PAGE_SIZE)
            {
                const u32 idx = bb.pagesUsed++;
                GPUPage* page = LH_NEW(Memory::Category::GPU, GPUPage);
                page->backingIndex       = i;
                page->pageIndexInBacking = idx;
                page->buffer             = bb.buffer;
                page->baseOffset         = static_cast<u64>(idx) * PAGE_SIZE;
                page->basePtr            = static_cast<u8*>(bb.mappedPtr) + page->baseOffset;
                page->tag                = tag;
                page->used               = 0;
                m_UsedPages.push_back(page);
                return page;
            }
        }

        // V6 overflow tier: all backings exhausted under steady GPU stall — grow.
        GrowBackingPoolLocked();
        if (m_BackingBuffers.empty()) return nullptr;

        BackingBuffer& bb = m_BackingBuffers.back();
        const u32 idx = bb.pagesUsed++;
        GPUPage* page = LH_NEW(Memory::Category::GPU, GPUPage);
        page->backingIndex       = static_cast<u32>(m_BackingBuffers.size() - 1);
        page->pageIndexInBacking = idx;
        page->buffer             = bb.buffer;
        page->baseOffset         = static_cast<u64>(idx) * PAGE_SIZE;
        page->basePtr            = static_cast<u8*>(bb.mappedPtr) + page->baseOffset;
        page->tag                = tag;
        page->used               = 0;
        m_UsedPages.push_back(page);
        return page;
    }

    void GPUTaggedPageAllocator::GrowBackingPoolLocked()
    {
        VkBufferCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        info.size  = BACKING_SIZE;
        info.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT
                   | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT
                   | VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT
                   | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT
                   | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
        // Backings carve into 2 MB pages distributed across all per-frame SSBO / UBO consumers; once forward-plus /
        // gpu-particles route their compute work to AsyncCompute, the buffer regions cross queue families. Apply
        // CONCURRENT across the whole pool — uniform with the large-one-shot path.
        VulkanContext::Get().ApplyConcurrentSharing(info);

        BackingBuffer bb;
        bb.alloc = VulkanAllocator::AllocateMappedSequentialBuffer(info, bb.buffer, &bb.mappedPtr);
        if (!bb.alloc)
        {
            LH_CORE_CRITICAL("GPUTaggedPageAllocator: backing-buffer allocation failed!");
            return;
        }
        bb.pagesUsed = 0;
        m_BackingBuffers.push_back(bb);
        // No MemoryTracker call here — VulkanAllocator already records Category::GPU
        // for the backing; double-counting would inflate the snapshot.
    }
}
