# v2.8.10 — gpu-tagged-heap

**Date:** 2026-04-29
**Commits:** 9 (on `feat/gpu-tagged-heap`)
**Issue:** [#103](https://github.com/Hekbas/Luth/issues/103)

---

## Overview

Built the GPU half of the Naughty Dog Onion/Garlic split. `Memory::GPUTaggedPageAllocator` (sibling to the existing CPU `TaggedPageAllocator`) vends 2 MB pages from 64 MB host-visible mapped backings; tag-based bulk-free fenced on GPU-N-2 timeline completion in `VulkanBackend::AcquireImage`. The four ring buffers from v2.8.9 (Material Set 2, Object Set 5, Indirect, `BoneMatrixBuffer` Set 4 — the latter deferred from v2.8.9 for the same architectural reason) all migrate onto allocator-returned regions; v2.8.9's slot-encoded fixed-pool design dissolves end-to-end.

Concretely, the slot encoding deletes: `obj.materialIndex` becomes 0-based, `firstInstance` becomes a clean count, `m_CurrentRenderSlot` and the slice-base term in 4 indirect-draw call sites delete, the `MaterialSlot::dirtyFramesRemaining` countdown deletes, and `gpu_cull.comp`'s `srcOffset` push-constant deletes (push range 108B → 104B). Sets 2/4/5 + the cull descriptor are rebound per-frame to the new regions, gated on `UPDATE_AFTER_BIND_BIT` (which required enabling `descriptorBindingStorageBufferUpdateAfterBind` at device creation — discovered live as a validation error on the first D3 boot).

The same `AcquireImage`-gated `FreeTag` driver completes the **CPU-side V6 wiring** as a free win. Pre-this-epic, `TaggedPageAllocator::FreeTag` had zero callsites and `JobContext::Allocator` was unassigned across the entire fiber pool — the CPU heap shipped with the engine in v1.0.0 but had been dead code ever since. Both halves of the Onion/Garlic split are now operational.

Tag-only release per the v2.8.5 internal-architecture policy.

---

## Architectural alignment

The plan-mode session that produced this epic was the first to apply the post-v2.8.9 plan-mode discipline added to CLAUDE.md (architecture-first, primitive-inventory before design, paste arch docs into the Plan agent prompt). Result: the design composes with every existing primitive instead of inventing new ones — `VulkanAllocator::AllocateMappedSequentialBuffer` (v2.8.9 helper) for backings, `VulkanContext::PushDeletion` (v2.8.7 SpinLock-corrected) for large-one-shot release, `Luth::SpinLock` (V1) for the heap's own lock, `JobContext` FLS for the per-fiber `GPUThreadCache`, `MemoryTracker::Category::GPU` for backing-buffer accounting (no double-counting — backings record through `VulkanAllocator`, the heap records only its own page metadata).

| Cornerstone | Composition |
|---|---|
| #1 Tagged allocators | `GPUTaggedPageAllocator` is sibling to `TaggedPageAllocator`; same shape (2 MB pages, per-fiber ThreadCache, tag-bulk-free, V6 overflow tier via growable backing pool) |
| #1 V1 SpinLock on hot path | Heap's `m_Lock` is `Luth::SpinLock`, held only on page-claim and `FreeTag` (< 100 cycles). CPU heap converted from `std::mutex` → `SpinLock` in the same epic. Bump path holds no lock — `ActivePage` is per-fiber |
| #2 Fiber yields, no OS blocking | Allocator non-blocking. `FreeTag` runs on main thread after the existing `m_FrameTimeline.Wait` already returned |
| #3 No `thread_local` | `GPUThreadCache` lives on `JobContext` alongside the existing CPU `Allocator` pointer |
| #4 No raw new/delete | Page metadata via `LH_NEW(Memory::Category::GPU, GPUPage)`. Backing VkBuffer through `VulkanAllocator` (records `Category::GPU` once) |
| #5 Vulkan 1.3 only | Reuses `m_FrameTimeline` (TimelineSemaphore). `VMA_MEMORY_USAGE_AUTO + HOST_ACCESS_SEQUENTIAL_WRITE_BIT + MAPPED_BIT` on backings |
| #6 Editor decoupling | Heap lives in `Luth.lib`. ProfilerPanel reads stats via `GetStats()` |

---

## Sub-tasks

| # | Slug | Commit | Notes |
|---|---|---|---|
| D1 | Wire CPU TaggedPageAllocator V6 | `c5f73cf` | `std::mutex` → `Luth::SpinLock`, static `Get()`, App-paired Init/Shutdown, JobContext gains `CpuCache`, `AcquireImage` drives `FreeTag(N-2)`. Closes a documented-but-unwired V6 hazard from arch/fiber-system.md. |
| D2 | GPUTaggedPageAllocator skeleton | `fab9f5f` | New class with `Init/Shutdown/Allocate/AllocateLargeTagged/FlushRegion/FreeTag/GetStats`. Init pre-allocates one 64 MB backing. JobContext gains `GpuCache`. VulkanBackend lifecycle. No consumers. |
| D3 | Migrate MaterialSystem | `b94b0e0` | Allocates `MAX_MATERIALS * MATERIAL_SIZE` per game stage via `AllocateLargeTagged` (~2.3 MB > PAGE_SIZE → dedicated VkBuffer per frame). Set 2 layout gains UPDATE_AFTER_BIND. `dirtyFramesRemaining` countdown deletes. `gameSlot` arg dropped from `Update`. `obj.materialIndex` 0-based. |
| — | Device feature fix | `c18096e` | Validation-error fix surfaced at first D3 boot. Enables `descriptorBindingStorageBufferUpdateAfterBind` at device creation; SampledImage's variant was already on, StorageBuffer needed its own bit. Covers Sets 2/4/5. |
| D4 | Migrate BoneMatrixBuffer | `0a08cf5` | Persistent CPU staging (LH_ALLOC, identity-init at boot) replaces persistent GPU mapped buffer; `Update()` allocates a 2 MB region per game stage, copies staging, rewrites Set 4. Per-block free-list + std::mutex stay (D6 carry-over). Closes the v2.8.9-deferred bone race. |
| — | Heap tag-mismatch fix | `7a5337b` | Bump path didn't invalidate cached `ActivePage` when `cache.CurrentTag` changed across stages — fiber reuse would land allocations in stale pages. D4 was the first consumer to surface it (D3 used `AllocateLargeTagged` exclusively). |
| D5 | Dissolve Object/Indirect slot-encoding | `d0b4dbe` | Atomic coupled change. `m_ObjectSSBO`/`m_IndirectBuffer` deleted; `m_ObjectRegion`/`m_IndirectRegion` allocated per render stage. `m_CurrentRenderSlot` + slice-base term in 4 indirect-draw sites + `gpu_cull.comp` `srcOffset` all deleted. Push range 108B → 104B. Set 5 + cull descriptor layouts gain UPDATE_AFTER_BIND. |
| — | Iterator-debug fix | `8f1500b` | MSVC `_ITERATOR_DEBUG_LEVEL=2` crash in `FreeTag` after first frame: `pop_back` invalidates cached `end()`, next compare trips iterator-compatibility check. Switched both halves of the heap to index-based loops. Surfaced live on D5 boot. |
| — | Cull descriptor pool fix | `8939056` | D5's cull descriptor set was still allocated through the shared `DescriptorAllocator` whose pool isn't `UPDATE_AFTER_BIND_POOL_BIT`-capable. Added a dedicated `m_CullDescPool` matching the Set 5 pattern. Surfaced as a validation error on the second D5 boot. |
| D6 | Strip dead remnants | `60f220c` | `FrameData::GameSlot()` / `RenderSlot()` deleted (zero callers post-D5). `MaterialSystem::MAX_MATERIALS` moved back to `private:` (was public for the deleted slice-base bake). |
| D7 | Observability + wrap-up | (this commit) | ProfilerPanel "GPU Tagged Heap" panel showing backing count, active/free pages, large one-shots, in-flight bytes. Version bump + this history file. |

---

## Memory in flight (steady state)

| Buffer | Per-frame allocation | Path | In-flight (3 frames) |
|---|---|---|---|
| Object SSBO | ~458 KB | bump (shares page with Indirect) | shared |
| Indirect | ~800 KB | bump (shares page with Object) | shared |
| Bones | 2 MB | bump (one full page) | 6 MB |
| Material | ~2.3 MB | `AllocateLargeTagged` (dedicated VkBuffer) | 6.9 MB |

Backings: 1 × 64 MB at startup, expected to stay at 1 in typical scenes (steady-state ~3-4 pages out of 32 in use across the three in-flight tags). Pressure path covered by `GrowBackingPoolLocked`.

---

## Bugs surfaced and fixed mid-implementation

Five mid-implementation fixes, all surfaced live by validation layers or runtime crashes — exactly the kind of diagnostics the design's reliance on `UPDATE_AFTER_BIND_BIT` and the heap's tag-page invariants are supposed to catch. None were latent from prior efforts.

- **`descriptorBindingStorageBufferUpdateAfterBind` not enabled** (`c18096e`). The bindless-texture set in v1.0.0 enabled `descriptorBindingSampledImageUpdateAfterBind`; storage buffers need their own bit and didn't have it.
- **Stale-tag `ActivePage` on bump path** (`7a5337b`). Architectural slip in the D2 design: the `Allocate` hot path didn't check `page->tag == cache.CurrentTag` before bumping. Fixed before D5 surfaces it on a second consumer.
- **MSVC `_ITERATOR_DEBUG_LEVEL=2` crash on swap-pop pattern** (`8f1500b`). Both halves of the heap had inherited the swap-and-pop iteration from `TaggedPageAllocator`'s original v1.0.0 form. The CPU heap had no consumers until this epic, so the bug had been latent for a year.
- **Cull descriptor pool not UPDATE_AFTER_BIND-capable** (`8939056`). D5 added the layout flag but left the pool allocation through the shared `DescriptorAllocator` (whose pool flags are fixed). Dedicated pool matching Set 5's pattern.

---

## Out of scope (deliberately deferred)

- **D6 lock-removal** on `MaterialSystem::m_Lock` and `BoneMatrixBuffer::m_Lock`. Per-frame upload moved to the heap shrinks the lock scope to slot-alloc only; `SpinLock` (or lock-free atomic stack) becomes feasible. Separate follow-up epic.
- **Buffer Device Address (BDA)**. Larger surface (every shader's SSBO declaration). Would let the heap drop per-frame `vkUpdateDescriptorSets`. Separate epic.
- **Dynamic descriptor offsets** (`STORAGE_BUFFER_DYNAMIC`). Not worth the pipeline-layout churn for v1; `vkUpdateDescriptorSets` cost is < 5 µs/frame.
- **Object SSBO multi-view sharing**. Currently per-`Execute` (per-view) allocation matches v2.8.9 wastage; not a regression.

---

## Build verification

Solution unchanged structurally; rebuilt Debug x64 after each sub-task. No new warnings. Validation clean under stress (60s soak with material slider drag + camera orbit + skinned animation + Frame Debugger captures). Cull pipeline push-constant range = 104B; Sets 2/4/5 + cull descriptor accept per-frame `vkUpdateDescriptorSets` without validation complaints under in-flight cmd buffers.

Memory delta (steady state): ~64 MB backing buffer (always allocated) + ~15 MB tagged regions in flight (3 frames × ~5 MB / frame). Net change vs v2.8.9: +64 MB backing reservation, ~9 MB in-flight tagged data is roughly the same shape as v2.8.9's persistent ring buffers.
