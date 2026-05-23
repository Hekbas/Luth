# rt-renderer.3-forward-plus — forward-plus

**Date:** 2026-05-22
**Commits:** 10 (on `feat/forward-plus`)
**Issue:** [#54](https://github.com/Hekbas/Luth/issues/54)
**Series:** `rt-renderer`, third effort. Mode A series-coalesced — `Version.h` PATCH bump to `v3.0.2`, tag-only, no Release.

---

## Overview

Third sub-effort of the `rt-renderer` v3.0.x arc. Replaces the fixed 64-light `LightUBO` with **Olsson 3D clustered lighting**: the view frustum is split into a `16 × 9 × 24 = 3456` cluster grid; one async-compute pass writes per-cluster view-space AABBs (logarithmic depth slicing); a second async-compute pass intersects every light against every cluster, atomic-packs per-cluster index lists, and writes a `(offset, count)` table; the PBR fragment shader computes its cluster ID from `gl_FragCoord` + Olsson-linearized depth and loops only the lights touching that 3D cell.

Outcome: point-light count is no longer capped. Practical limit is now per-cluster overflow at `k_MaxLightsPerCluster = 128` (a tunable). Per-fragment cost scales with local light density rather than total scene density — a hallway with a near light and a far light at the same screen XY don't dump both into every fragment's loop the way 2D tile-based culling would.

Set 3 reshape required pushing the descriptor set per-view (cluster grid + light index differ between Scene + Game panel views). Two new compute pipelines + one debug-viz graphics pipeline land on `LightingSubsystem`. No new memory allocator, no new sync primitive, no new descriptor pool category that didn't already exist — composes with `GPUTaggedPageAllocator` + `per-frame-descriptor-set-cycling` + the async-compute queue topology + the producer-handle-flow rule from A.2.

---

## Sub-tasks

| # | What landed | Commit |
|---|---|---|
| A | **`GlobalUniforms` + `viewportSize`/`nearZ`/`farZ`.** Three fields appended (one std140 vec4 slot, 16 B). `GlobalSubsystem::UpdateUBO` pulls `nearZ`/`farZ` from `RenderView::camera`, viewport from `ViewResources::width/height`. 14 GLSL mirrors updated across all Set 0 b0 consumers. Foundation for cluster ID math. | [`27c4172`](../../../../commit/27c4172) |
| B | **SSBO structs.** `LightSSBOHeader` (48 B, std430), `GPUCluster` (8 B = uvec2), `k_ClusterTilesX/Y/SlicesZ` + `k_MaxLightsPerCluster` constants added to `LightTypes.h`. `static_assert` on each. Header-only — no consumers yet. | [`111740d`](../../../../commit/111740d) |
| C | **Set 3 shadow b1 → b3.** Layout grows from 2 → 4 bindings (b0 UBO + b1/b2 stub SSBOs + b3 shadow CIS). `UploadLightUBO` writes a shared 16 B stub region to b1/b2 each frame; `pbr.frag` shadow binding moves to b3. Render-identical bisect milestone — shadows still work, b1/b2 inert until F. | [`1ab07aa`](../../../../commit/1ab07aa) |
| D | **`ClusterBuildPass`.** `cluster_build.comp` computes per-cluster view-space AABB via Olsson log depth slicing; unprojects NDC tile corners through `invProjection` PC, intersects with z-planes, AABBs of 8 corner points. `VKComputePipeline` + 2-binding UAB layout (AABB write, Grid write) on `LightingSubsystem`. Per-view `clusterBuildDescSet[N]` on `ViewResources` cycled per frame; pool gains `STORAGE_BUFFER` category. Dispatch `(4, 3, 6)` groups of `(4, 4, 4)` on async-compute queue. | [`6c6acf1`](../../../../commit/6c6acf1) |
| E | **`LightAssignPass`.** `light_assign.comp` sphere-vs-AABB per cluster; `atomicAdd(counter, localCount)` reserves contiguous slots in LightIndex; `(offset, count)` written to ClusterGrid. 5-binding UAB layout on `LightingSubsystem`, per-view `lightAssignDescSet[N]`. Cluster handles flow through `ClusterBuildOutputs` struct (handles + SubRegions), consumed by `builder.ReadBuffer(handle)` — no re-`ImportBuffer` per A.2 hazard 1. | [`07d3964`](../../../../commit/07d3964), fixes [`08c40b3`](../../../../commit/08c40b3), [`6749d37`](../../../../commit/6749d37) |
| F | **Promote Set 3 b0 UBO → SSBO + cluster lookup in `pbr.frag`.** `LightUniforms` deleted; `LightGatherer` flips to `GatheredLights` (DirectionalLightData + `std::vector<PointLightData>`) — capacity reused across frames, no per-frame heap alloc. `CaptureSnapshot` drops the 64-cap. `pbr.frag` declares `LightSSBO` (header + flexible `points[]`, std430) + `ClusterGrid` (b1) + `LightIndex` (b2); new `ComputeClusterID` function; light loop = cluster lookup → loop `oc.y` indices into `points[]`. Set 3 moves to `ViewResources::lightDescSet[N]` because cluster grid + light index are per-view. `UploadLightUBO` splits into `UploadLightSSBO` (before LightAssign, so the compute pass can bind the same VkBuffer to its b0 read) + `WriteSet3PerView` (after, completes b0/b1/b2). | [`3053220`](../../../../commit/3053220) |
| G | **`ClusterVizPass` (true 3D density) + ShadeMode + ProfilerPanel + UI radio.** `cluster_viz.frag` samples `SceneDepth`, linearizes via Olsson, computes the per-fragment 3D cluster ID, fetches the cluster's light count, and heat-maps over LDR (sky pixels pass through). Two-set pipeline: set 0 = depth sampler (per-view, written once at `AllocateViewResources` via `WriteClusterVizView`); set 1 = the existing lightDescSet (only b1 ClusterGrid read by the shader). New `ShadeMode::ClustersDensity` entry, gated dispatch from BuildGraph after SlimViz. ScenePanel Debug split adds a "Forward+ Clusters" subsection with a `Cluster Density` radio. ProfilerPanel "Active point lights" row from `LightingSystem::GetLights().points.size()`. | [`2c00325`](../../../../commit/2c00325) |
| H | **Wrap-up.** This history file. `ROADMAP.md` row → done. `arch/rendering-pipeline.md` Set 3 row + target render graph updated; new RG hazard documented. `Version.h` 3.0.1 → 3.0.2. `--no-ff` merge into `main` + `v3.0.2` tag. Mode A — tag-only, no Release. | this commit |

---

## Bugs caught during smoke testing

Two bugs in sequence on `feat/forward-plus`; both fixes preserved as separate commits:

1. **Out-of-bounds `BufferHandle` deref ([`08c40b3`](../../../../commit/08c40b3)).** `AddLightAssignPass` resolved producer handles via `rg.GetBuffers()[cb.aabb.index]` — but `BufferHandle.index` is 1-based (0 is the invalid sentinel; see `RenderGraph.cpp:195`). At runtime this read one past the AABB buffer (returning the Grid node) and then tried to read index 4 in a size-4 vector, triggering `STATUS_BREAKPOINT` at debug `vector::operator[]`. Fix: subtract 1. Existing RG internal sites (`m_Buffers[handle.index - 1]`) already use this idiom.

2. **Lost SubRegion offsets in cluster bindings ([`6749d37`](../../../../commit/6749d37)).** `BufferHandle` → `BufferNode` only carries the backing `VkBuffer` pointer; the slice offset+size live on the `GPUSubRegion` returned by `GPUTaggedPageAllocator::Allocate`. `AddLightAssignPass` was binding `(buffer, offset=0, size=desc.size)` for the AABB + Grid descriptors — pointing the compute writes at the *start* of the backing VkBuffer, trampling Material SSBO / ObjectSSBO data that lived at the same offsets. Manifested as geometry flicker (matflags + bindless indices corrupted per frame; matID stable because it's a different field in `GPUObjectData`). Fix: thread `aabbRegion` + `gridRegion` through `ClusterBuildOutputs`, bind with `(region.buffer, region.offset, region.size)`. Existing internal code (`GeometrySubsystem.cpp:563-566`) already followed this pattern — the bug was in *new* code that took a shortcut via the RG handle lookup.

Both bugs root from the same misconception: treating `BufferHandle` as if it carried enough info to describe a binding. Lesson: BufferHandles describe RG resource nodes for barrier tracking, not physical buffer slices for descriptor writes. The producer keeps the `GPUSubRegion`; consumers receive it via the producer's return struct. Documented as a new RG hazard in `arch/rendering-pipeline.md`.

---

## Architectural decisions

### Set 3 moves to ViewResources

The legacy `LightUniforms` was view-independent (lights are world-space), so a single global `m_LightDescSet[N]` worked. With Forward+, the cluster grid + light index ARE per-view (different projection → different cluster AABBs → different per-cluster light intersections). Three resolutions considered:

- **Keep Set 3 global, write per-view buffers into the global slot just-in-time.** Each view's `RecordView` would overwrite the previous view's Set 3 b1/b2 mid-frame. Validation 03047 territory; cycling alone doesn't help because both views write the same slot.
- **Move Set 3 b1/b2 to a different set, keep b0 on a global Set 3.** Splits the Lighting domain across descriptor sets. Doesn't match the subsystem-owns-its-set rule from `arch/rendering-pipeline.md`.
- **Move all of Set 3 to ViewResources.** One subsystem still owns the *layout* + the *pipelines* + the *shadow map asset*; per-view data sits on per-view storage. Matches the GTAO pattern (per-view `gtaoMainDescSet`).

Option 3 won. `LightingSubsystem::GetLightDescSet(slot)` is now a one-liner delegating to `m_Pipeline->GetCurrentViewResources()->lightDescSet[slot]` — callers in `GeometrySubsystem` / `EditorOverlaysSubsystem` / `FrameDebuggerContext` don't change.

### `UploadLightSSBO` + `WriteSet3PerView` split

`LightAssignPass` reads the LightSSBO via its own compute descriptor set's binding 0 — same VkBuffer the per-view Set 3 b0 points at (tagged-heap backings carry both UBO + SSBO usage bits). Both bindings need to be set up before the compute pass dispatches and the PBR draws sample.

But `WriteSet3PerView` for Set 3 b1 + b2 needs the *outputs* of the cluster passes (the LightIndex region only exists after `AddLightAssignPass` returns). Chicken-and-egg.

Split into two phases:
- `UploadLightSSBO(lights)` → allocates + populates the LightSSBO region, caches it on `m_LastLightSSBORegion` for the LightAssign compute to bind. Runs *before* `AddClusterBuildPass`.
- `WriteSet3PerView(lightSSBORegion, clusterGridRegion, lightIndexRegion)` → writes Set 3 b0/b1/b2 to the per-view slot. Runs *after* `AddLightAssignPass` returns its `LightAssignOutputs`.

Shadow sampler (b3) is stable across slots and frames; `WriteShadowView(vr)` writes it once at `AllocateViewResources` time and propagates to every cycled slot.

### Cluster grid + AABB writes share the heap, but with correct offsets

A.2's first hazard ("re-importing a VkImage aliases it onto two ResourceNodes") has a buffer-side analogue that bit hard this effort. Tagged-heap allocations all return slices of the same large backing `VkBuffer`. `rg.ImportBuffer(desc, region.buffer, ...)` only sees the VkBuffer pointer, not the slice offset. Two import calls for AABB + Grid create two RG nodes both pointing at the same VkBuffer — which is FINE for RG state tracking (the two slices don't overlap), but you have to remember the offsets came from the SubRegions, not the RG nodes.

The bug-fix pattern is now in the codebase: producer returns `BufferHandle` + `SubRegion`, consumer uses SubRegion fields for `VkDescriptorBufferInfo`. Documented in `arch/rendering-pipeline.md` as a hazard so the next async-compute-on-tagged-heap effort doesn't repeat it.

### 3D depth-sampled cluster viz over per-tile aggregation

First-pass `cluster_viz.frag` was a 2D screen-tile heat-map collapsing 24 Z-slices to a max-per-column value, with a separate "tile bounds" outline mode. The 2D viz didn't actually exercise the 3D clustering — it just aggregated counts across slices, which made the viz inseparable from a tile-based renderer's debug viz. The bounds mode added little (a fixed 16×9 screen grid) and didn't show Z-slice boundaries.

Rebuilt as a true 3D viz before merging: `cluster_viz.frag` samples `SceneDepth` to derive the per-fragment Olsson slice → real cluster ID → cluster's count. Sky / far-plane pixels stay transparent. Pipeline grows to two descriptor sets (depth sampler + lightDescSet); per-view `clusterVizDescSet` written once at `AllocateViewResources`. `ClustersBounds` mode dropped — the heat-map's tint discontinuities at cluster boundaries are visible in the depth-sampled viz when needed.

A world-space cluster-wireframe diagnostic mode (render cluster AABBs as filtered wireframes via DebugDraw, color by count) is the natural follow-up if this debug viz proves insufficient under stress scenes — out of scope for A.3.

### 64-cap cleanup spans `LightUniforms`, `LightGatherer`, *and* `CaptureSnapshot`

The plan-mode review flagged the cap in `LightUniforms` and `LightGatherer`. `CaptureSnapshot` had a *third* copy of the cap at the snapshot-capture stage ("cap 64, matching LightUniforms"). Found mid-sub-task-F; dropped along with the others. Lesson: when a literal constant exists in multiple sites, grep before declaring it eliminated.

---

## Files touched

**Engine (Luth.lib):**
- [`core/RenderSnapshot.cpp`](../../../luth/source/luth/core/RenderSnapshot.cpp) — drop 64-cap; unbounded `pointLights` span
- [`renderer/lighting/LightTypes.h`](../../../luth/source/luth/renderer/lighting/LightTypes.h) — delete `LightUniforms`; add `LightSSBOHeader`/`GPUCluster`/cluster constants + `GatheredLights` (DirectionalLightData + std::vector<PointLightData>)
- [`renderer/lighting/LightGatherer.{h,cpp}`](../../../luth/source/luth/renderer/lighting/LightGatherer.h) — `Gather` signature → `GatheredLights&`; vector resize reuses capacity
- [`scene/systems/LightingSystem.h`](../../../luth/source/luth/scene/systems/LightingSystem.h) — `m_Lights` → `GatheredLights`
- [`scene/systems/RenderingSystem.h`](../../../luth/source/luth/scene/systems/RenderingSystem.h) — `GlobalUniforms` + viewport/nearZ/farZ; `ShadeMode::ClustersDensity`
- [`scene/systems/RenderingSystem.cpp`](../../../luth/source/luth/scene/systems/RenderingSystem.cpp) — remove global `UploadLightUBO` call (now per-view inside `BuildGraph`)
- [`renderer/subsystems/GlobalSubsystem.cpp`](../../../luth/source/luth/renderer/subsystems/GlobalSubsystem.cpp) — populate viewport/nearZ/farZ from `RenderView::camera` + `ViewResources`
- [`renderer/subsystems/LightingSubsystem.{h,cpp}`](../../../luth/source/luth/renderer/subsystems/LightingSubsystem.h) — Set 3 layout 2 → 4 bindings; pool/sets move to ViewResources; `UploadLightSSBO` + `WriteSet3PerView` + `WriteShadowView`; `ClusterBuild` + `LightAssign` compute pipelines + descriptor layouts; `ClusterViz` graphics pipeline (depth sampler + 2-set layout) + `AddClusterVizPass` + `WriteClusterVizView`; `GetLightDescSet(slot)` delegates to current view; SubRegion threading in `ClusterBuildOutputs` / `LightAssignOutputs`
- [`renderer/RenderPipeline.{h,cpp}`](../../../luth/source/luth/renderer/RenderPipeline.h) — `ViewResources::lightDescSet` + `clusterBuildDescSet` + `lightAssignDescSet` + `clusterVizDescSet`; BuildGraph wires `UploadLightSSBO → AddClusterBuildPass → AddLightAssignPass → WriteSet3PerView` + gated `AddClusterVizPass(rg, ldrOutput, prepassDepth)`; removes `UploadLightUBO` forwarder
- [`renderer/ViewResources.cpp`](../../../luth/source/luth/renderer/ViewResources.cpp) — `k_ViewPoolMaxSets` 32 → 48; `k_ViewPoolStorageBufferCount` = 48; alloc light/cluster/assign/viz sets; call `WriteShadowView` + `WriteClusterVizView`

**Shaders:**
- [`cluster_build.comp`](../../../luth/assets/shaders/cluster_build.comp) (NEW)
- [`light_assign.comp`](../../../luth/assets/shaders/light_assign.comp) (NEW)
- [`cluster_viz.frag`](../../../luth/assets/shaders/cluster_viz.frag) (NEW — reuses `fullscreen.vert`; samples SceneDepth for 3D cluster ID)
- [`pbr.frag`](../../../luth/assets/shaders/pbr.frag) — Set 3 declarations (b0 SSBO + b1 ClusterGrid + b2 LightIndex + b3 shadow); `ComputeClusterID`; cluster-lookup light loop
- Mechanical `GlobalUniforms` block updates across 14 existing shaders (every shader binding Set 0 b0)

**Editor (Luthien.lib):**
- [`panels/ScenePanel.cpp`](../../../luthien/source/luthien/panels/ScenePanel.cpp) — `Cluster Density` radio under "Forward+ Clusters" subsection in Debug split
- [`panels/ProfilerPanel.{h,cpp}`](../../../luthien/source/luthien/panels/ProfilerPanel.h) — `m_PointLightCount` + row

**Docs:**
- [`arch/rendering-pipeline.md`](../arch/rendering-pipeline.md) — Set 3 row (4 bindings; per-view); target render graph (cluster passes wired in + ClusterViz path); new RG hazard documented (BufferHandle vs SubRegion offsets)
- [`epics/rt-renderer.md`](../epics/rt-renderer.md) — A.3 Progress Tracker row → done (local, untracked)
- [`ROADMAP.md`](../../ROADMAP.md) — v3.0.2 row

---

## Verification

Build clean on every commit (0 errors, pre-existing warnings only). Smoke milestones on `feat/forward-plus`:

- **After E (LightAssign):** ClusterBuild + LightAssign visible in frame debugger with GPU timer rows. No validation errors after the BufferHandle + SubRegion fixes landed.
- **After F (pbr.frag cluster loop):** Scenes with ≤ 64 lights render identical to pre-effort baseline. Shadows still work via Set 3 b3 → per-view `lightDescSet`. Multi-view (Scene + Game panel) shows no contamination — each view's cluster passes feed its own LightIndex.
- **After G (ClusterViz):** Scene panel Debug split shows the `Cluster Density` radio under a "Forward+ Clusters" subsection. Toggling shows the depth-sampled heat-map — geometry tinted by per-fragment cluster count, sky pass-through. ProfilerPanel "Active point lights" row updates at 10 Hz with point-light count.

No banding at cluster boundaries — the cluster ID formula in `cluster_build.comp` and `pbr.frag` are kept in lockstep through shared `k_ClusterTilesX/Y/SlicesZ` constants.
