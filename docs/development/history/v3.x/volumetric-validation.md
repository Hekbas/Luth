# volumetric-validation

**Date:** 2026-05-23
**Commits:** 4 (on `fix/rg-depth-handoff`, squashed before merge from 6 — instrumentation + cleanup pair folded since they were net-zero diff)
**Issue:** [#131](https://github.com/Hekbas/Luth/issues/131)
**Series:** `rt-renderer` follow-up. Mode A series-coalesced — `Version.h` PATCH bump to `v3.0.4`, tag-only, no Release.

---

## Overview

Closes the three Vulkan validation errors that the volumetric-fog effort (v3.0.3) shipped as known issues, plus a spec-compliance fix the investigation surfaced. Originally scoped as `fix: rg-depth-handoff` after the v3.0.3 history mis-diagnosed VUID-vkCmdDraw-None-09600 as a sceneDepth cross-queue layout-handoff bug. Instrumentation revealed the actual cause is in the volumetric inject/composite descriptor declarations — the offending VkImage in the validator's complaint was the shadow map array (`arrayLayer=3`), not sceneDepth. The issue + branch were renamed mid-effort to `volumetric-validation`; the branch name `fix/rg-depth-handoff` stuck as a git artifact.

Four bugs fixed:

1. **Image barriers omit `VK_QUEUE_FAMILY_IGNORED`.** `RenderGraph::Execute`'s `VkImageMemoryBarrier2` construction left `srcQueueFamilyIndex` / `dstQueueFamilyIndex` zero-initialized (= graphics family index on this hardware). For `VK_SHARING_MODE_CONCURRENT` images (the per-resource opt-in policy at `arch/multi-queue.md`), the spec requires both indices to be `VK_QUEUE_FAMILY_IGNORED` per `VUID-VkImageMemoryBarrier2-image-04071`. Buffer barriers in the same `Execute` loop already set IGNORED; 30+ other emission sites across `FrameDebugger.cpp` / `FrameDebuggerContext.cpp` do too. Only the central RG path was missing it.
2. **`VolumetricInjectPass` samples the shadow map without declaring the read.** Setup lambda declared `WriteStorageImage(volDensity)` + `WriteStorageImage(volInScatter)` but never called `builder.Read` on the per-cascade shadow handles, so the solver never emitted `DSA → SHADER_READ_ONLY` barriers per cascade. ShadowPass writes left the cascades in DSA; inject sampled them at the wrong layout → VUID-09600 on cascade 3 (the last one written, hence the first the validator surfaces).
3. **`VolumetricCompositePass` samples `volInScatter` without declaring the read.** Same shape as #2 — composite's setup declared `Write(sceneColor)` + `Read(sceneDepth)` only, missing the sampler-binding-1 read on `volInScatter`. The integrate pass writes inScatter as a storage image (GENERAL); without composite's declared read the solver never transitions back to SHADER_READ_ONLY → VUID-09600 on the color image.
4. **`VolumetricInject` descriptor rewritten while pending.** `WriteInjectPerFrame` rewrites bindings 2-5 on the cycled `volInjectDescSet` each frame, but the layout didn't have `VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT`. VUID-vkUpdateDescriptorSets-None-03047 fired ×4 per frame. The cluster_build / light_assign layouts in `LightingSubsystem` already use this pattern — straight copy.

The shadow-handle plumbing required threading `shadowHandles[]` from `BuildGraph` into `AddInjectPass`, and threading the inject pass's atlas handles into `AddIntegratePass` (instead of re-importing the same `VkImage` — that's the v3.0.1 `slim-gbuffer` smoke hazard #1 to avoid). `AddIntegratePass` now returns its post-write `volInScatter` handle so `AddCompositePass` can declare its sampler read.

---

## Sub-tasks

The table lists the 4 commits as they appear on `main` after squash. Two pre-squash commits — instrumentation add + cleanup remove — were folded since the net diff is zero. The investigation arc those commits enabled is documented in "Architectural decisions" below.

| # | What landed | Commit |
|---|---|---|
| 1 | **`VK_QUEUE_FAMILY_IGNORED` fix + arch hazard entry.** Added `srcQueueFamilyIndex = dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED` to RG image pre/post barrier emission. Dropped two stale "known issue" comments (`RenderPipeline.cpp`, `VolumetricSubsystem.cpp`) that pointed at the misdiagnosed sceneDepth cause. Appended 4th hazard entry to `arch/rendering-pipeline.md`. Spec-compliance fix; on its own merits but did NOT silence VUID-09600 (the actual cause was discovered next). | `cbd85d1` |
| 2 | **Volumetric VUID fixes.** `AddInjectPass` takes `shadowHandles[]` and declares per-cascade reads. Returns `InjectOutputs { density, inScatter }`. `AddIntegratePass` takes those handles (no re-`ImportResource`), returns post-write `inScatter`. `AddCompositePass` takes the inScatter handle and declares its sampler read. Inject descriptor layout bindings 2-5 get `UPDATE_AFTER_BIND` flag + layout `UPDATE_AFTER_BIND_POOL` flag. `BuildGraph` plumbs all handles through. | `d509377` |
| 3 | **Inject shadow read uses COMPUTE_SHADER stage.** Switched the per-cascade shadow `builder.Read` to `builder.ReadStorageImage`. `Read` maps to `ShaderResource` → FRAGMENT_SHADER stage (graphics-only); inject runs on the compute queue. `ReadStorageImage` maps to `ComputeRead` → COMPUTE_SHADER, with the same SHADER_READ_ONLY target layout. The builder method name is about queue affinity, not descriptor type — the COMBINED_IMAGE_SAMPLER descriptor works fine with either layout source. Silences VUID-09676 on the compute primary. | `b43ea88` |
| Wrap-up | **History + version bump + merge.** This document. `Version.h` 3.0.3 → 3.0.4. `--no-ff` merge into `main` + `v3.0.4` tag. | this commit |

---

## Architectural decisions

### Instrument first, fix from evidence

The original brief diagnosed VUID-09600 as a sceneDepth cross-queue layout-tracking bug in `RenderGraph::SolveBarriers`. A Plan agent proposed the `VK_QUEUE_FAMILY_IGNORED` fix as the most-likely cause. Rather than ship on speculation, sub-task 1 added a targeted log filtered to sceneDepth and the user ran the engine to capture a trace. The trace immediately ruled out the speculation: VkImage in the validator's error was `0x8a...` with `arrayLayer=3`, while sceneDepth was logged as `0x34...` — completely different images. The shadow map (4-layer D32 array) was the only depth image with `arrayLayer > 0`. From there the actual cause (undeclared inject + composite reads) was straightforward to identify in the code.

Cost: ~95 LOC of instrumentation that's now removed. Benefit: avoided shipping a "spec compliance fix that didn't fix the user-visible bug" with no follow-up plan. The IGNORED fix still landed (sub-task 2) as legitimate independent work — buffer barriers + every other emission site already follow the IGNORED pattern; the central RG path was the outlier.

### Inject → integrate handle chaining (arch hazard #1 cleanup)

The pre-fix inject and integrate passes each called `rg.ImportResource(...)` for the same `volDensity` / `volInScatter` `VkImage`. This is the v3.0.1 `slim-gbuffer` smoke hazard #1 — aliasing the same `VkImage` onto two `ResourceNode`s diverges the barrier solver's state tracking (each node has its own `currentState` / `lastWriter`). Inject's write left its node in `ComputeWrite`; integrate's separate node tracked its own state independently.

While fixing the missing reads, the inject pass was changed to return `InjectOutputs { density, inScatter }` and integrate now takes those handles via `ReadStorageImage` / `WriteStorageImage` on the same nodes. Composite similarly takes integrate's returned handle. Matches the existing pattern (`SlimGBufferOutput`, `ClusterBuildOutputs`, `GeometryPassOutput`) and resolves the hazard for the volumetric chain.

### `ReadStorageImage` despite COMBINED_IMAGE_SAMPLER descriptor

The fix uses `builder.ReadStorageImage(shadowHandles[i])` even though the inject shader's binding-6 descriptor is `COMBINED_IMAGE_SAMPLER`. The builder method's name describes queue affinity (`ComputeRead` → `COMPUTE_SHADER` stage), not descriptor type. The target layout (`SHADER_READ_ONLY_OPTIMAL`) works for both sampler and storage reads — `ComputeRead` and `ShaderResource` map to the same layout, only the stage mask differs. Renaming `ReadStorageImage` → `ComputeRead` (or adding a `ReadCompute` alias) is a clarity follow-up for a future RG polish effort; out of scope here.

---

## Known issues / follow-ups

### `isCrossQueue` only checks `lastWriter`, not last-toucher

Surfaced incidentally by the instrumentation: when a graphics pass writes a depth image AFTER a compute pass reads it, the solver doesn't tag the barrier as cross-queue. The trace shows `GeometryPass(g) write ComputeRead->DSA  xq=0` — `xq=0` despite the previous touch (`GTAODepthPrefilter`) being on the compute queue. `SolveBarriers::isCrossQueue` only checks `lastWriter`, and reads don't update `lastWriter`. So when GeometryPass writes after GTAOPrefilter reads, the solver thinks the previous touch was on the same queue (SlimGBufferPass, the previous graphics writer).

The barrier still emits the correct layout transition (`SHADER_READ_ONLY → DSA`), but with `srcStage = COMPUTE_SHADER_BIT` (from the `ComputeRead` before-state). The cross-queue semaphore at submit time provides the actual execution dependency, so this is a semantically-wrong-but-functionally-harmless barrier. The TOP_OF_PIPE substitution that `crossQueueSrc=true` would trigger is the documented arch pattern; we're skipping it for the read-then-write-cross-queue case.

Not fixed in this effort because (a) it didn't cause any of the user-reported VUIDs, (b) the fix would require tracking a `lastReader` field alongside `lastWriter` in `ResourceNode`, which is a foundational change beyond the scope of this effort. Worth picking up in a future RG hardening pass if a real validation regression surfaces.

### Validator note

Verification at launcher state (pre-project-load): no VUID firing. Verification after project load: no VUID firing — confirms the original "no VUIDs after project load" state is preserved (the post-load case was always silent because resize triggers `EnsureViewResources` reallocation which had its own latent self-fixing properties).

---

## Bugs caught during smoke testing

1. **`Read` on compute queue trips VUID-09676.** First iteration of the inject-shadow-read fix used `builder.Read(shadowHandles[i])`, which maps to `ShaderResource` → `VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT`. Inject runs on the compute queue, which doesn't support FRAGMENT_SHADER. Fixed by switching to `builder.ReadStorageImage` (sub-task 2c follow-up). The builder name remains a clarity issue (see "Architectural decisions" above).

---

## Build verification

- Debug x64 builds clean across all 6 commits — only pre-existing warnings (LNK4006 dbghelp, C4996 getenv/strncpy, C4244 Editor chrono conversion).
- User-side smoke test at launcher state: zero VUIDs after the final fix.
- User-side smoke test after project load: zero VUIDs.
- Frame Debugger pass list shows `VolumetricInject` (async-compute) declaring per-cascade shadow reads; `VolumetricComposite` declaring its volInScatter sampler read. The full barrier chain is visible in the existing `m_GraphSnapshot` capture surface.

### Tagging

After this commit merges to `main`: `git tag -a v3.0.4 -m "v3.0.4 — volumetric-validation"` + `git push --follow-tags`. Mode A — tag-only, no GitHub Release.
