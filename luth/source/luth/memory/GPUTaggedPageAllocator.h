#pragma once

#include "luth/core/types/LuthTypes.h"
#include "luth/jobs/SpinLock.h"
#include <vector>
#include <vulkan/vulkan.h>

// Forward-declare VMA types to keep header lightweight
typedef struct VmaAllocation_T* VmaAllocation;

namespace Luth::Memory
{
    // GPU half of the Naughty Dog Onion/Garlic split. Sibling to TaggedPageAllocator:
    // 2 MB pages carved from larger HOST_VISIBLE | MAPPED VkBuffers (backings),
    // tag-based bulk-free fenced on GPU N-2 timeline completion.
    // See docs/development/arch/memory.md.

    // Sub-region returned to consumers — bind via descriptors / use as indirect-buffer offset.
    struct GPUSubRegion
    {
        VkBuffer  buffer    = VK_NULL_HANDLE;
        u64       offset    = 0;
        u64       size      = 0;
        void*     mappedPtr = nullptr;  // already shifted by `offset`
    };

    struct GPUPage
    {
        u32  backingIndex       = 0;
        u32  pageIndexInBacking = 0;
        u64  used               = 0;
        u32  tag                = 0;
        bool isLargeOneShot     = false;
        bool isDeviceLocal      = false;  // Garlic large-one-shot: DEVICE_LOCAL, non-mapped (basePtr == nullptr)

        // Cached for fast Allocate hot path
        VkBuffer buffer     = VK_NULL_HANDLE;
        u64      baseOffset = 0;
        void*    basePtr    = nullptr;

        // Set only when isLargeOneShot — the dedicated VkBuffer that this "page" owns.
        VmaAllocation oneShotAlloc = nullptr;
    };

    // Per-fiber cache (lives on JobContext, not TLS — see arch/fiber-system.md).
    struct GPUThreadCache
    {
        GPUPage* ActivePage = nullptr;
        u32      CurrentTag = 0;
    };

    class GPUTaggedPageAllocator
    {
    public:
        static constexpr u64 PAGE_SIZE    = 2 * 1024 * 1024;   // matches CPU heap
        static constexpr u64 BACKING_SIZE = 64 * 1024 * 1024;  // 32 pages per backing

        GPUTaggedPageAllocator();
        ~GPUTaggedPageAllocator();

        // Singleton — VulkanBackend owns lifecycle (post-VulkanContext::Init, pre-Shutdown).
        static GPUTaggedPageAllocator& Get();

        void Init();
        void Shutdown();

        // Hot path — bumps within active page; claims a new page on overflow.
        // alignment is rounded up to max(alignment, m_MinAlignment).
        // size > PAGE_SIZE delegates to AllocateLargeTagged.
        GPUSubRegion Allocate(GPUThreadCache& cache, u64 size, u64 alignment = 16);

        // For requests that exceed PAGE_SIZE: dedicated VkBuffer per request, tag-released.
        GPUSubRegion AllocateLargeTagged(u32 tag, u64 size, u64 alignment = 16);

        // Garlic sibling of AllocateLargeTagged: DEVICE_LOCAL, non-mapped, for GPU-only read+write
        // buffers (ReSTIR reservoirs, SVGF history) that would thrash PCIe in the host-visible heap.
        // Same tag/FreeTag/recycle lifetime; returned region's mappedPtr is null. see arch/memory.md
        GPUSubRegion AllocateLargeTaggedDeviceLocal(u32 tag, u64 size, u64 alignment = 16);

        // Wraps vmaFlushAllocation on the backing; no-op on HOST_COHERENT memory.
        void FlushRegion(const GPUSubRegion& region);

        // Bulk-release all pages tagged `tag`. Driven from VulkanBackend::AcquireImage
        // after the GPU N-2 timeline wait (V6 — see arch/fiber-system.md).
        void FreeTag(u32 tag);

        struct Stats
        {
            u32 BackingBuffers   = 0;
            u32 ActivePages      = 0;
            u32 FreePages        = 0;
            u32 LargeOneShots    = 0;
            u32 FreeLargePages   = 0;
            u64 BytesInFlight    = 0;
        };
        Stats GetStats() const;

    private:
        struct BackingBuffer
        {
            VkBuffer       buffer    = VK_NULL_HANDLE;
            VmaAllocation  alloc     = nullptr;
            void*          mappedPtr = nullptr;
            u32            pagesUsed = 0;  // monotonic; reset only at GrowBackingPool re-init
        };

        // m_Lock held. Pulls from m_FreePages or carves a new page from a backing buffer
        // (allocating a new backing via GrowBackingPool when all are full).
        GPUPage* AllocatePageLocked(u32 tag);
        void GrowBackingPoolLocked();

        std::vector<BackingBuffer> m_BackingBuffers;
        std::vector<GPUPage*>      m_FreePages;
        std::vector<GPUPage*>      m_UsedPages;   // includes large-one-shot pages
        std::vector<GPUPage*>      m_FreeLargePages;  // recycled large-one-shots: VkBuffer kept alive, `used` = capacity, tag cleared
        std::vector<GPUPage*>      m_FreeLargeDeviceLocalPages;  // device-local recycle pool, kept disjoint so a host-visible buffer never satisfies a device-local request
        Luth::SpinLock             m_Lock;        // V1: hot path SpinLock, NOT std::mutex
        u64                        m_MinAlignment = 16;
    };
}
