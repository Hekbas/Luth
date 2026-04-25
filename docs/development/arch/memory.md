# Memory — Allocators, Tracker, STL Gap

## Constraint

Engine hot paths must not call `new`/`delete` directly. Allocations are categorized at call sites for budget visibility.

---

## Allocators

| Allocator | Owner | Lifetime | Use case |
|-----------|-------|----------|----------|
| `LinearAllocator` | per-frame, per-thread | bump until `Reset()` | Frame-temp data inside a single fiber's stack of work (DrawList scratch, transform propagation buffers) |
| `TaggedPageAllocator` | global pool | until `FreeTag(tag)` | Frame-tagged data spanning fibers (e.g. all "Frame N" allocations released when GPU finishes N-2) |
| VMA (`VulkanContext`) | global | until VMA shutdown | GPU-resident buffers / images (out of CPU tracker scope; tracked under `Category::GPU` only at allocation moment) |

**`LinearAllocator`** ([luth/source/luth/memory/LinearAllocator.h](../../../luth/source/luth/memory/LinearAllocator.h)) — page-based bump pointer. First page allocated at construction with the requested size. On overflow, allocates a new default-sized page (or `max(default, size+align)` if the request exceeds default). `Reset()` rewinds to first page without freeing pages — pages stay live for reuse next frame.

**`TaggedPageAllocator`** ([luth/source/luth/memory/TaggedPageAllocator.h](../../../luth/source/luth/memory/TaggedPageAllocator.h)) — 2 MB pages from `VirtualAlloc` (Windows), pooled in `m_FreePages`. Per-thread `ThreadCache` holds the active page + current tag, eliminating contention on the hot allocate path. `FreeTag(tag)` linearly scans `m_UsedPages` and returns matching pages to the free pool. Pages-per-tag is small (~10–20 per frame at typical use), so linear scan is fine.

**Hazard V6** (GPU stall ↔ allocator reset deadlock — see [arch/fiber-system.md](fiber-system.md)) is the reason an *overflow tier* exists for `TaggedPageAllocator`: when the GPU stalls on Frame N-2, Frame N's tag still gets pages, and the overflow pool absorbs the pressure until N-2 completes.

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
| `GPU` | VMA allocations (GPU-resident) |

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
