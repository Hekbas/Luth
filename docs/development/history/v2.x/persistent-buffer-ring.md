# v2.8.9 — persistent-buffer-ring

**Date:** 2026-04-28
**Commits:** 4 (on `feat/persistent-buffer-ring`)
**Issue:** [#102](https://github.com/Hekbas/Luth/issues/102)

---

## Overview

Triple-buffered the three persistent CPU-mapped SSBOs whose writers were racing the GPU: `m_ObjectSSBO` (Set 5), `m_IndirectBuffer`, and `MaterialSystem::m_Buffer` (Set 2). Before this effort, all three were single-buffered. The swapchain frame fence in `AcquireImage` only waits for `frameIndex - MAX_FRAMES_IN_FLIGHT + 1` (= GPU(N-3) completion); under the v2.8.4 pipelined model (Game(N) | Render(N-1) | GPU(N-2)) frames N-1 and N-2 stay in flight on the GPU while frame N's CPU writers fill the same buffer. The race showed up as per-frame flicker on rapid material edits and was the most visible "Tier-1" Vulkan-correctness gap left after `vulkan-correctness` (v2.8.7).

The shipped design was the simplest of the three options I'd considered (single buffer 3× sized + slice math vs. three buffers + per-frame descriptor swap vs. `STORAGE_BUFFER_DYNAMIC` + dynamic offsets): one VkBuffer per resource sized `stride × MAX_FRAMES_IN_FLIGHT`, with the slice base baked into the existing CPU-side index paths (`firstInstance`, cull push-constants, `obj.materialIndex`). Zero shader changes, descriptors stay `VK_WHOLE_SIZE` and written once at init. The only new shader push-constant — a 4-byte `srcOffset` on `gpu_cull.comp` — was a correctness fix surfaced mid-implementation: the cull's object SSBO read needed its own slice base separate from the indirect destination offset.

VMA modernization landed bundled in the final commit: `VMA_MEMORY_USAGE_CPU_TO_GPU` (deprecated) → `VMA_MEMORY_USAGE_AUTO` + `HOST_ACCESS_SEQUENTIAL_WRITE_BIT` + `MAPPED_BIT` for the three ring buffers, with an unconditional `vmaFlushAllocation` per-slice after each writer (no-op on `HOST_COHERENT` memory).

Tag-only release per the post-v2.8.5 policy.

---

## Critical timing detail (the source of the bug)

The two writers operate on different pipeline stages and therefore see different ring slots:

- **`MaterialSystem::Update`** runs in **game stage** (asserts `Stage::Game` at `MaterialSystem.cpp:95`; called from `RenderSnapshot.cpp:194`). Its writes are intended for GPU frame N (consumed by Render(N) in the next iteration). Active slot = `gameSlot = frameIndex % 3`.
- **`RenderPipeline::BuildGPUObjectBuffer`** runs in **render stage** (called from `RenderingSystem::Update` at line 175, after `RenderFrame().Snapshot` access). Its writes are intended for GPU frame N-1 (consumed by Render(N-1) being recorded right now). Active slot = `renderSlot = renderFrameIndex % 3 = (frameIndex - 1) % 3`.

With `MAX_FRAMES_IN_FLIGHT = 3` the two slots always differ in steady state, so `Game(N)` writing the materials slice never aliases `Render(N-1)` writing the object slice — they target different ring positions concurrently. The slice that GPU(N-1) eventually consumes was prepared by `Game(N-1)` in the prior iteration (materials slice `(N-1)%3`) and `Render(N-1)` in this iteration (objects slice `(N-1)%3`) — the same slot index in both buffers. So the slice base baked into `obj.materialIndex` uses **renderSlot**, not gameSlot: it identifies the GPU-frame this object is being prepared for, which matches the materials slice that frame consumes.

This is the answer to the subtlety I spent the most time on — initial sketches had `obj.materialIndex` keyed off gameSlot, which would have shifted the read to the wrong slice.

---

## Design

### Storage layout

One VkBuffer per resource, allocated 3× the original size. Slice base is computed by the writer and baked into the existing offset paths:

| Buffer | Slice base path | Field shifted |
|---|---|---|
| ObjectSSBO | `objects[gl_BaseInstance]` | `firstInstance = renderSlot * k_MaxGPUObjects + count` (CPU-side, in `BuildGPUObjectBuffer`) |
| IndirectBuffer | `cmdIndex * sizeof(VkDrawIndexedIndirectCommand)` | `cmdIndex = (renderSlot * k_IndirectRegionCount + viewBaseRegion + cascadeOffset) * k_IndirectRegionStride + dc.gpuObjectIndex` (3 draw callsites + cull push-constants) |
| Material SSBO | `materials[obj.materialIndex]` | `obj.materialIndex = renderSlot * MAX_MATERIALS + matSlot` (CPU-side, in `BuildGPUObjectBuffer`) |

Descriptors stay `VK_WHOLE_SIZE`, written once at init. No descriptor-set churn, no pipeline-layout breakage. Vulkan validation has no opinion on the slice — every offset/index lands within the buffer.

### Cull `srcOffset` push-constant

The cull compute shader reads `objects[idx]` where `idx = gl_GlobalInvocationID.x` (0..objectCount-1). Without a slice base, this would always read slot 0's objects. Adding `destOffset` (which writes commands into the active slice's region) doesn't help — `destOffset` is in command-units and the indirect buffer's region layout, while objects need a slice base in object-units (`renderSlot * k_MaxGPUObjects`). Conflating them was tempting (one offset per slot) but the scaling factor differs.

Solution: a 4-byte `srcOffset` field appended to `CullPushConstants`. Push-constant range grows from 104B → 108B in the cull pipeline layout; the cull shader's read becomes `objects[pc.srcOffset + idx]`. Write side stays `commands[pc.destOffset + idx]` because `destOffset` already encodes both the slice base and the per-region offset within the slice.

### Material dirty-frame countdown

`MaterialSlot::dirty` (bool) → `u8 dirtyFramesRemaining`. Any change-detected upload (re-armed by either `IsGpuDirty()` or the existing `memcmp(oldData, newData)` check) sets the counter to `MAX_FRAMES_IN_FLIGHT`. Each `Update` call writes the active gameSlot slice and decrements the counter. After 3 game-stage iterations the change has propagated to all three slices.

`IsGpuDirty()` is cleared on the **first** detected change (when the counter is set), not on the last write — otherwise a sticky flag would re-arm the countdown on every iteration and the system would memcpy 16384 material slots per frame forever. The `memcmp` safety net catches the case where `IsGpuDirty()` was never set but the underlying GPU data changed via `UpdateGPUData()` (e.g. a bindless texture finished streaming and the index changed).

### VMA modernization

`AllocateMappedSequentialBuffer` is a focused new entry point on `VulkanAllocator` for ring-style upload paths. It bundles the modern flag set:

```cpp
VmaAllocationCreateInfo allocInfo = {};
allocInfo.usage = VMA_MEMORY_USAGE_AUTO;
allocInfo.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT
                | VMA_ALLOCATION_CREATE_MAPPED_BIT;
```

`MAPPED_BIT` makes VMA own the persistent map: `pMappedData` comes back via the `VmaAllocationInfo` out-param at create time, and `vmaDestroyBuffer` auto-unmaps. The legacy `Map`/`Unmap` API stays put for the dozens of other allocations across the engine — only the three ring buffers migrate.

`FlushSlice(allocation, offset, size)` wraps `vmaFlushAllocation`. VMA inspects the underlying memory type and short-circuits on `HOST_COHERENT`, so the wrapper is unconditional — no `needsFlush` cache (a micro-optimization that wasn't worth carrying). Called once per writer at the end (twice in `BuildGPUObjectBuffer` for the Object slice + Indirect slice; once in `MaterialSystem::Update` for the Material slice).

---

## Sub-tasks

| # | Slug | Commit | Notes |
|---|---|---|---|
| A | Plumb slot accessors | `9c9cec3` | `FrameData::GameSlot()` / `RenderSlot()` added; threaded through `BuildGPUObjectBuffer` and `MaterialSystem::Update` as unused params (no behavior change). Validates the plumbing. |
| B | Triple-buffer ObjectSSBO + IndirectBuffer | `0c3808c` | 3× alloc, slice math at the writer, cull dispatch (with new `srcOffset` push-constant), 3 indirect-draw callsites, frame-debugger replay path. Material SSBO still single-buffered at the end of this commit — `obj.materialIndex` keeps the 0-based `matSlot`. |
| C | Triple-buffer Material SSBO | `6167450` | 3× alloc, dirty-frame countdown, gameSlot slice writes; `obj.materialIndex` baked with `renderSlot * MAX_MATERIALS + matSlot`. |
| D | VMA modernization + wrap-up | (this commit) | `AllocateMappedSequentialBuffer` + `FlushSlice` API on `VulkanAllocator`; switch the three ring allocations off deprecated `VMA_MEMORY_USAGE_CPU_TO_GPU`. Drop explicit Map/Unmap for these three. Version bump + this history file. |

---

## Changes

| File | Change |
|---|---|
| `luth/source/luth/core/FrameData.h` | EDIT — add `GameSlot()` / `RenderSlot()` accessors |
| `luth/source/luth/core/RenderSnapshot.cpp` | EDIT — pass `frameData.GameSlot()` to `MaterialSystem::Update` |
| `luth/source/luth/scene/systems/RenderingSystem.cpp` | EDIT — pass `frameData.RenderSlot()` to `BuildGPUObjectBuffer` |
| `luth/source/luth/renderer/RenderPipeline.h` | EDIT — `BuildGPUObjectBuffer(snapshot, renderSlot)` signature; cache `m_CurrentRenderSlot` |
| `luth/source/luth/renderer/RenderPipeline.cpp` | EDIT — set `m_CurrentRenderSlot` at Execute(); 3× indirect `BufferDesc`; cull dispatch slice math (camera + 4 cascades) + `srcOffset`; drop ring-buffer Unmap calls in Shutdown |
| `luth/source/luth/renderer/gpu/GPUObjectBuffers.cpp` | EDIT — 3× alloc; slice math in `BuildGPUObjectBuffer` (`firstInstance`, indirect baseCmd, `obj.materialIndex`); `FlushSlice` for both buffers; switch to `AllocateMappedSequentialBuffer`; cull pipeline push-constant range 104B → 108B |
| `luth/source/luth/renderer/passes/CullPass.h` | EDIT — `AddCullComputePass` gains `u32 srcOffset` parameter |
| `luth/source/luth/renderer/passes/CullPass.cpp` | EDIT — `CullPushConstants::srcOffset` field; push to shader |
| `luth/source/luth/renderer/passes/DepthPrepass.cpp` | EDIT — indirect-draw offset shifted by slice base |
| `luth/source/luth/renderer/passes/GeometryPass.cpp` | EDIT — indirect-draw offset shifted by slice base |
| `luth/source/luth/renderer/passes/ShadowPass.cpp` | EDIT — indirect-draw offset shifted by slice base (per cascade) |
| `luth/source/luth/renderer/debug/FrameDebuggerContext.cpp` | EDIT — replay-path indirect offset shifted by slice base |
| `luth/source/luth/renderer/material/MaterialSystem.h` | EDIT — `MAX_MATERIALS` made public; `MaterialSlot::dirty` (bool) → `u8 dirtyFramesRemaining` |
| `luth/source/luth/renderer/material/MaterialSystem.cpp` | EDIT — 3× alloc, gameSlot slice writes, dirty-frame countdown, `FlushSlice`; switch to `AllocateMappedSequentialBuffer`; drop Unmap |
| `luth/source/luth/renderer/backend/vulkan/VulkanAllocator.h` | EDIT — add `AllocateMappedSequentialBuffer` + `FlushSlice` |
| `luth/source/luth/renderer/backend/vulkan/VulkanAllocator.cpp` | EDIT — implement the two new entry points |
| `luth/assets/shaders/gpu_cull.comp` | EDIT — `srcOffset` push-constant; `objects[srcOffset + idx]` |
| `luth/source/luth/core/Version.h` | EDIT — bump to 2.8.9 |

---

## Out of scope (deliberately)

- **`BoneMatrixBuffer` (Set 4) has the same race.** Flagged during exploration but not in scope per the spec. Lives in `renderer/resources/`, uses a free-list block allocator that may need a different ring shape (per-block-allocation slice indices, not a single buffer). Future follow-up.
- **`MaterialSystem` lock removal.** The `std::mutex` retains for now (per the v2.8.4 stage-isolation D6 carry-over). Conversion to lock-free is its own effort.
- **Debug-only "writing to in-flight slice" assertion.** The plan called this optional; with `MAX=3` and the staged-writer design (game vs. render different slots in steady state) the invariant is structural, not a runtime-detectable fault. Adding the assert would need the steady-state guard (frameIndex ≥ 2) and meaningful payoff isn't there. Skipped.

---

## Build verification

Solution unchanged; rebuilt Debug x64 after each commit. No new warnings, no Vulkan validation errors at startup.

Memory delta (when populated): `+9 MB` total at steady state (Object 0.5 → 1.5 MB; Indirect 0.8 → 2.4 MB; Material 2.3 → 6.9 MB). Tracked under `Memory::Category::GPU` via the existing `MemoryTracker::RecordAlloc` hook.

The cull shader source change (`gpu_cull.comp`) re-cooks via the shader asset pipeline on first runtime startup (source mtime > artifact mtime → recompile). New SPIR-V picks up the `srcOffset` push-constant; the host-side `VkPushConstantRange.size` was bumped to match.

---

## Bugs surfaced and fixed mid-implementation

- **Cull's object SSBO read missed the slice base.** Initial sketch passed only `destOffset` (command index). The compute shader's `objects[idx]` would always read slot 0 — invisible until `m_GPUObjectCount > 0` and the camera moves enough to make the per-frame state diverge. Caught while reviewing the cull shader; resolved by adding `srcOffset` to the push-constants.
- **Frame Debugger replay path was a 4th indirect-draw site.** Missed during the initial spec scoping (the plan listed only the 3 main passes). Caught by `grep vkCmdDrawIndexedIndirect`; updated `FrameDebuggerContext::ReplayBatch` to use `m_CurrentRenderSlot * k_IndirectRegionCount` consistent with live `GeometryPass`.
- **`MAX_MATERIALS` was private.** `obj.materialIndex` is baked in `BuildGPUObjectBuffer` (lives outside `MaterialSystem`). Made the constant `public` rather than adding a getter — simpler, lower-risk, and the constant has no other consumers that would benefit from the indirection.

None of these were latent bugs from prior efforts — all introduced and resolved within this branch.
