# Memory — Allocators, Tracker, STL Gap

## Constraint

Engine hot paths must not call `new`/`delete` directly. Allocations are categorized at call sites for budget visibility.

---

## Allocators

| Allocator | Owner | Lifetime | Use case |
|-----------|-------|----------|----------|
| `LinearAllocator` | per-frame, per-thread | bump until `Reset()` | Frame-temp data inside a single fiber's stack of work (DrawList scratch, transform propagation buffers) |
| `TaggedPageAllocator` | global pool (CPU heap) | until `FreeTag(tag)` | Frame-tagged data spanning fibers (released when the consuming frame's GPU work completes) |
| `GPUTaggedPageAllocator` | global pool (GPU heap) | until `FreeTag(tag)` | GPU half of the Onion/Garlic split — per-frame SSBO regions (Material, Object, Indirect, Bones) released when the consuming frame completes |
| VMA (`VulkanContext`) | global | until VMA shutdown | GPU-resident buffers / images (out of CPU tracker scope; tracked under `Category::GPU` only at allocation moment) |

**`LinearAllocator`** ([luth/source/luth/memory/LinearAllocator.h](../../../luth/source/luth/memory/LinearAllocator.h)) — page-based bump pointer. First page allocated at construction with the requested size. On overflow, allocates a new default-sized page (or `max(default, size+align)` if the request exceeds default). `Reset()` rewinds to first page without freeing pages — pages stay live for reuse next frame.

**`TaggedPageAllocator`** ([luth/source/luth/memory/TaggedPageAllocator.h](../../../luth/source/luth/memory/TaggedPageAllocator.h)) — 2 MB pages from `VirtualAlloc` (Windows), pooled in `m_FreePages`. Per-fiber `ThreadCache` (lives on `JobContext`, not `thread_local`) holds the active page + current tag, eliminating contention on the hot allocate path. `FreeTag(tag)` linearly scans `m_UsedPages` and returns matching pages to the free pool. Pages-per-tag is small (~10–20 per frame at typical use), so linear scan is fine. Hot-path lock is `Luth::SpinLock` (V1).

**`GPUTaggedPageAllocator`** ([luth/source/luth/memory/GPUTaggedPageAllocator.h](../../../luth/source/luth/memory/GPUTaggedPageAllocator.h)) — sibling to the CPU heap. 2 MB pages carved from 64 MB host-visible mapped `VkBuffer` backings (allocated via `VulkanAllocator::AllocateMappedSequentialBuffer`). Per-fiber `GPUThreadCache` on `JobContext`. Allocations exceeding `PAGE_SIZE` route through `AllocateLargeTagged` — a dedicated `VkBuffer` tagged like a page, **recycled** into an internal pool on `FreeTag` (never destroyed on reclaim — a too-early free is stale data, not an unmapped-VA fault, matching the page path), reused on a same-size alloc, destroyed only at `Shutdown`. Backing pool grows on V6 pressure (`GrowBackingPoolLocked`). `MemoryTracker` records under `Category::GPU` once per backing buffer (delegated to `VulkanAllocator`); the heap itself records only its own page metadata to avoid double-counting.

**Device-local (Garlic) path.** The page pool + `AllocateLargeTagged` above are the **Onion** half (host-visible, CPU-writes-sequentially → GPU-reads): Material / Object / Indirect / Bone / per-frame UBOs. `AllocateLargeTaggedDeviceLocal` is the **Garlic** sibling for GPU-only **read+write** buffers (ReSTIR reservoirs) — host-visible memory would thrash PCIe on a discrete GPU. (The SVGF denoiser's history is GPU-only read+write too, but uses persistent VMA `VKTexture` **images**, not this buffer path — à-trous + reprojection need bilinear taps, so it follows the `restirDI` / `taaHistory` per-view-image precedent.) It allocates `DEVICE_LOCAL` non-mapped buffers via `VulkanAllocator::AllocateBuffer(..., AUTO_PREFER_DEVICE)`, recycled by tag in a pool kept **disjoint** from the host-visible large pool (a same-size host-visible buffer must never satisfy a device-local request), and is exempt from `FlushRegion` (nothing mapped). Same tag/`FreeTag` lifetime — but **persistent** consumers (per-view ping-pong retained for the view's lifetime) use a reserved tag outside the per-frame range and `FreeTag` only on resize, not on the N-2 sweep.

**V6 driver wiring.** Both halves of the Onion/Garlic split are driven from a single site — `VulkanBackend::AcquireImage`. It block-waits the retiring frame's cached timeline values (CPU backpressure), then runs a **direct completion sweep**: for each consuming-frame `label` that `IsFrameComplete(label)` confirms GPU-done, `FreeTag(label-1)` on both heaps — the submit labeled `L` consumed all data tagged `L-1`. **Tag rule:** allocations are tagged with the consuming frame (render-stage producers use `GetRenderFrameIndex()`; game-stage producers use `GetFrameIndex()`). Pre-`gpu-tagged-heap` (v2.8.10) the CPU heap shipped without this driver — `FreeTag` had zero callsites and `JobContext::Allocator` was unassigned across the entire fiber pool — so the CPU heap was effectively dead code. The same epic that built the GPU heap also operationalized the CPU one.

**Hazard V6** (GPU stall ↔ allocator reset deadlock — see [arch/fiber-system.md](fiber-system.md)) — both heaps absorb pressure by growing their pool: `TaggedPageAllocator` calls `VirtualAlloc` for a fresh page, `GPUTaggedPageAllocator` calls `GrowBackingPoolLocked` for a fresh 64 MB backing. Frame N gets pages even when GPU stalls on N-2.

---

## MemoryTracker

`Luth::Memory::MemoryTracker` ([luth/source/luth/memory/MemoryTracker.h](../../../luth/source/luth/memory/MemoryTracker.h)) is the in-engine runtime stats counter. Lock-free atomic counters per category, snapshot-readable from the editor for in-game UI overlays.

### Categories

| Category | Tracks |
|----------|--------|
| `General` | Default / untagged |
| `Rendering` | Vulkan wrappers, pipelines, buffers, textures (CPU side) |
| `Scene` | ECS, entities, components |
| `Jobs` | Fibers, deques, job data |
| `Resources` | AssetManager, importers, loaded asset data |
| `Editor` | ImGui, panels, editor-only |
| `FrameLinear` | `LinearAllocator` page allocations |
| `FrameTagged` | `TaggedPageAllocator` `VirtualAlloc` pages |
| `GPU` | VMA allocations (GPU-resident) — includes `GPUTaggedPageAllocator` backing buffers |

### API

```cpp
MemoryTracker::RecordAlloc(Category, size);  // hot path — atomic adds
MemoryTracker::RecordFree(Category, size);
MemoryTracker::Snapshot s = MemoryTracker::GetSnapshot();  // for UI
```

`Snapshot` is plain data: per-category `{Current, Peak, Total, Allocs, Frees}` plus `TotalCurrent`/`TotalPeak`. `Current` is signed (`i64`) so underflow bugs surface as negative values rather than silent wraparound.

---

## Tracked allocation macros

Allocations are **opt-in**. Source code declares a category at each call site via `LH_NEW` / `LH_ALLOC` / `LH_NEW_ARRAY` (and matching free macros) defined in [`MemoryMacros.h`](../../../luth/source/luth/memory/MemoryMacros.h):

```cpp
auto* ctx = LH_NEW(Memory::Category::Rendering, VulkanContext);
LH_DELETE(Memory::Category::Rendering, ctx);

void* buf = LH_ALLOC(Memory::Category::Resources, sizeBytes);
LH_FREE(Memory::Category::Resources, buf, sizeBytes);
```

Each macro records to `MemoryTracker` *and* fires `LH_PROFILE_ALLOC` / `LH_PROFILE_FREE` (Tracy memory zone — see [profiling.md](profiling.md)).

---

## STL gap (intentional)

`std::vector`, `std::unordered_map`, `std::string`, etc. allocate via the default `std::allocator` and **bypass `MemoryTracker`**. Same for third-party libraries (Assimp, ImGui, GLFW, VMA, Tracy itself).

This means the in-engine `MemoryTracker` snapshot reports **engine-boundary deliberate allocations only**. It is *not* a complete picture of process memory.

### Why we accept this

| Option considered | Rejected because |
|-------------------|------------------|
| Override global `operator new`/`delete` and assign all to `Category::General` | Third-party allocations dominate the count and have no actionable signal — pure noise, fakes the appearance of comprehensive coverage |
| Provide tracked STL allocators (`LH::Vector<T>`) and migrate every call site | Multi-week refactor across 100+ sites; some libs (EnTT internal containers) can't be replaced; cost outweighs solo-dev value |
| Accept gap, document it, capture untracked allocations via Tracy memory profiler instead | **Chosen.** Tracy's `TracyAlloc`/`TracyFree` global hooks (wired in v2.8.2 `engine-consolidation`) capture *every* allocation including STL/third-party at capture time, with full callstack. `MemoryTracker` keeps its narrow role: runtime category stats for in-engine UI |

### Coverage matrix

| Source of allocation | `MemoryTracker` | Tracy memory profiler |
|----------------------|-----------------|-----------------------|
| `LH_NEW` / `LH_ALLOC` call sites | ✅ category-tagged | ✅ category-named zone |
| `LinearAllocator` / `TaggedPageAllocator` page acquires | ✅ `FrameLinear` / `FrameTagged` | ✅ |
| VMA GPU allocations | ✅ `GPU` (at allocation) | ✅ |
| `std::vector` / `std::string` / `std::unordered_map` growth | ❌ untracked | ✅ via global `operator new` hook |
| Third-party (Assimp, ImGui, GLFW) | ❌ untracked | ✅ via global `operator new` hook |

For *capture-time deep memory analysis*, attach Tracy. For *runtime per-category budgets visible in the editor*, use `MemoryTracker::GetSnapshot()`.

---

## When to revisit

A future epic could introduce tracked STL allocators (`LH::Vector<T>` + propagation through component arrays / asset caches) if:
- Profiling shows STL container churn dominates a hot path
- A specific subsystem needs per-call-site category breakdown beyond what Tracy callstacks provide
- The engine grows beyond solo-dev scope and team coordination requires stricter conventions

Until then, the gap is named, not hidden.
