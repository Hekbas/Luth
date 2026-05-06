# v2.9.13 — per-frame-descriptor-set-cycling

**Date:** 2026-05-06
**Branch:** `fix/per-frame-descriptor-set-cycling`
**Mode:** B (per-effort tag, tag-only — internal correctness fix, no Release)
**Estimate:** M

---

## Overview

Replaces the v2.9.11 UAB-on-single-set workaround with rotated descriptor sets across `MAX_FRAMES_IN_FLIGHT` (= 3) slots. Each frame writes its own slot; no descriptor write ever races a still-bound set. UAB flags then come off every cycled binding/layout/pool. Pure correctness: rotate storage, index at bind, drop UAB. Zero behavior change in rendered output.

v2.9.11 made the per-frame UBO race spec-valid by enabling `UPDATE_AFTER_BIND` and migrating UBOs to `GPUTaggedPageAllocator`. That was correct but papered over the underlying issue: the descriptor *set* itself was single-instance, with every frame rewriting the same `VkDescriptorSet` and relying on UAB late-write semantics for safety. The cycling refactor makes the set/slot rotation match the heap-region rotation, so no late-write semantics are required for the affected bindings.

v2.9.12 (`render-pipeline-subsystems`) was the structural prerequisite — each cycled descriptor Set's full lifecycle now lives in one subsystem file, so this fix was per-subsystem mechanical instead of an 18-file hunt.

---

## Sub-Tasks (commit log)

| # | Commit | Subject |
|---|--------|---------|
| 1 | `a7324ed` | fix(renderer): cycle Light descriptor set (Set 3) |
| 2 | `ac09305` | fix(renderer): cycle Set 5 + Cull descriptor sets |
| 3 | `aded7c2` | fix(renderer): cycle Set 0 + Grid; resize per-view pool |
| 4 | `9ba1b8b` | fix(renderer): cycle GTAO main per-view (extends Pair T) |
| 5 | `ab137ed` | fix(renderer): cycle 4 PostProcess sets; drop UAB pool flag |
| 6 | `5610b3e` | fix(renderer): pin captured slot for FrameDebugger replay |

6 implementation commits + this wrap-up. Each commit ends in build-clean state with Vulkan validation expected to pass.

---

## What changed

### Storage shape

Every cycled descriptor-set member becomes `std::array<VkDescriptorSet, MAX_FRAMES_IN_FLIGHT>`. Affected sites:

| Set | Storage | Owning subsystem |
|---|---|---|
| Set 0 (per-view) | `ViewResources::globalDescriptorSet` | Global |
| Set 3 (single-global) | `LightingSubsystem::m_LightDescSet` | Lighting |
| Set 5 (single-global) | `GeometrySubsystem::m_ObjectSSBODescSet` | Geometry |
| Cull (single-global) | `GeometrySubsystem::m_CullDescSet` | Geometry |
| 4 PP (per-view) | `ViewResources::{bloomExtract,bloomBlurH,bloomBlurV,composite}DescSet` | PostProcess |
| GTAO main (per-view) | `ViewResources::gtaoMainDescSet` | GTAO |
| Grid (per-view) | `ViewResources::gridDescSet` | EditorOverlays |

Sets 1 (bindless), 2 (Material), 4 (BoneMatrix) untouched — different lifecycle. GTAO prefilter, GTAO denoise, Outline per-view sets untouched — no per-frame UBO binding, no UAB to begin with.

### Indexing rule

Every per-frame `Update*UBO` / `Upload*UBO` / `BuildGPUObjectBuffer` caches the slot at function entry, immediately adjacent to the existing `CurrentTag` assignment so they share the single `GetRenderFrameIndex()` read:

```cpp
const u32 frameAbs = static_cast<u32>(Renderer::GetFrameData()->GetRenderFrameIndex());
const u32 slot     = frameAbs % MAX_FRAMES_IN_FLIGHT;
jobCtx->GpuCache.CurrentTag = frameAbs;   // absolute index drives heap tag
// ... slot drives descriptor-array storage
```

`m_RenderFrameIndex` is a plain `u64` mutated only by `App::Run` between render-stage dispatches — immutable for the duration of a render stage. Caching defends future refactors.

Bind sites compute the slot inline at lambda body entry and index `[slot]` directly; no helper. Cull-pass executor switches from by-value `descSet` capture to `[this, ...]` capture and recomputes slot at execution time (capturing by value would freeze slot 0).

### Cross-set atomicity preserved

Three batched writes share one heap region per frame. Both/all writes use the same cached `slot`:

- **Pair G** — `GlobalSubsystem::UpdateUBO` writes Set 0 binding 0 + Grid binding 0 in one batched call.
- **Pair T** — `GTAOSubsystem::UpdateUBO` writes Set 0 binding 5 + GTAO main binding 2 in one batched call.
- **Pair P** — `PostProcessSubsystem::UpdateUBO` writes binding 2 of all 4 PP sets in one batched call.

Comments at each writer's heap-region invariant got an extension: "AND the same per-frame slot — both writes use the same `slot` so the next frame's allocator doesn't overwrite a region the previous frame's binding still references". Existing comments are load-bearing.

### Allocation flow

Per-cycled-group allocation: one `vkAllocateDescriptorSets` call with `descriptorSetCount = MAX_FRAMES_IN_FLIGHT` and a layouts array filled with the same handle. `ViewResources::AllocateViewResources` splits the existing alloc lambda into `allocSingle` (non-cycled) and `allocCycled` (3-set bulk).

### `WriteView` fan-out

Stable bindings (samplers, IBL textures, GTAO sampler, depth sampler) propagate to all 3 slots inside each `WriteView` / `WriteGridView` / `CreateShadowResources` initial-write site. One outer `for (u32 s = 0; s < MAX_FRAMES_IN_FLIGHT; ++s)` per writer; same `pImageInfo` reused across slots (descriptors copied, not aliased). Resize path (`RenderPipeline::EnsureViewResources`) is gated by `Renderer::WaitForGPU` before re-entering `WriteView` so no in-flight cmd buffer references the new sets at write time.

### UAB removed per binding

Every cycled binding drops `VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT`. Each owning layout drops `VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT`. Each owning pool drops `VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT`. The per-view shared pool drops UAB last (Commit 5) once the final UAB layout (PP) is gone — verified safe because GTAO prefilter / denoise / Outline never had UAB on their layouts.

### Pool sizing

Per-view pool: cycled sets allocate `MAX_FRAMES_IN_FLIGHT` instances each. New constants:

| Constant | Old | New |
|---|---|---|
| `k_ViewPoolMaxSets` | 16 | 32 |
| `k_ViewPoolUniformBufferCount` | 12 | 32 |
| `k_ViewPoolCombinedSamplerCount` | 32 | 64 |
| `k_ViewPoolStorageImageCount` | 12 | 8 |

Storage-image count drops because none of those bindings are cycled. Dedicated subsystem pools (`m_LightDescPool`, `m_ObjectSSBODescPool`, `m_CullDescPool`) all bump `maxSets` 1 → 3 and per-binding count ×3.

### FrameDebugger Frozen replay

`CapturedFrame` gains `u32 capturedRenderFrameIndex`, stamped at `BeginCapture` from the live render-frame index. `ReplayGeometry` indexes by `capturedRenderFrameIndex % MAX_FRAMES_IN_FLIGHT` instead of live `GetRenderFrameIndex()` — Frozen state pins the slot the captured UBOs/SSBOs reference, not whatever the live loop has rotated to. Stub replays (`ReplayShadow`, `ReplayDepthPrepass`, `ReplaySelectionMask`) inherit the same field when implemented.

---

## Architectural alignment

- **Cornerstone 1 (per-frame data through tagged allocators):** unchanged. Every cycled descriptor still sources its UBO/SSBO from `Memory::GPUTaggedPageAllocator::Get()` — only the *descriptor* is now slot-indexed; the heap region is still tagged with the absolute frame index.
- **Cornerstone 5 (no legacy Vulkan):** unchanged. `vkAllocateDescriptorSets` bulk allocation is core. Removing UAB doesn't reintroduce any deprecated path.
- **`FreeTag(N-2)` invariant:** preserved. Heap-region reclamation operates on absolute tags; descriptor slot rotation operates on the modulo. Slot reuse distance (3, gated by `m_FrameTimeline.Wait(N - MAX_FRAMES_IN_FLIGHT + 1)` in `VulkanBackend::AcquireImage`) is stricter than `FreeTag` distance (2), so descriptor cycling is at least as safe as heap reclamation.
- **`arch/rendering-pipeline.md`:** descriptor table footnote updated — cycled bindings note slot rotation, UAB list reduced to Set 1 (bindless) + Sets 2/4 (Material/BoneMatrix).

---

## Build verification

Debug x64 builds clean per commit (0 errors, pre-existing warnings only — `LNK4006` from `vulkan-1.lib` symbol overlap with `shaderc_shared.dll`/`ws2_32.dll`/`dbghelp.dll`, `C4244` chrono conversion in `Editor.cpp:619`). Validation layers smoke-test deferred to user's runtime gate (next).

---

## Smoke gate

Before merging to `main`, the user runs the full smoke test (per memory `feedback_smoke_gate_before_merge.md`). This is a correctness fix with zero visible UX change, so the bar is "did anything regress and does validation stay clean". Targets:

- Drag editor camera 60+ s in Lit / Unlit / Wireframe — no `VUID-vkUpdateDescriptorSets-*` errors, no "descriptor set updated while bound" warnings.
- Game panel + Scene panel concurrent (14 cycled `VkDescriptorSet`s in flight per frame across both views) — validates per-view-pool sizing.
- Toggle PostProcess settings (`bloomStrength`, `tonemapOp`, `chromaticAberration`) during motion — exercises Pair P every frame.
- Toggle GTAO on/off twice during motion — exercises Pair T cycled writes.
- Frame Debugger capture, hold Frozen 10+ s while wiggling the editor camera; verify GeometryPass per-draw preview stays stable on the captured slot. Recapture from Frozen.
- Hot-reload `pbr.frag`, `shadowDepth.vert`, `bloomBlur.frag`, `outline.frag` — pipelines rebuild, no descriptor warnings.
- Long-session memory: `GPUTaggedPageAllocator` working set steady-state ~6 MB (matches v2.9.12 baseline).

Pass criteria:
- Zero `VUID-vkUpdateDescriptorSets-*` validation messages.
- Zero "descriptor set updated while bound" warnings.
- Zero `VK_ERROR_OUT_OF_POOL_MEMORY`.
- Render output unchanged vs v2.9.12 (Lit/Unlit/Wireframe parity, bloom/GTAO/grid visually identical).
- Frame Debugger preview stable across long Frozen hold.
- Performance neutral — same `vkUpdateDescriptorSets` call count per frame, only `dstSet` slot index differs.

Once the user gives go-ahead, merge + tag run as:

```
git checkout main
git merge --no-ff fix/per-frame-descriptor-set-cycling -m "feat(release): merge fix/per-frame-descriptor-set-cycling"
git tag -a v2.9.13 -m "v2.9.13 — per-frame-descriptor-set-cycling"
git push origin main --follow-tags
```

Tag-only — no `gh release create` (Mode B internal — see CLAUDE.md "Tagging vs. releasing").

---

## Outstanding follow-ups

1. **Stub replays gain captured-slot indexing.** `ReplayShadow`, `ReplayDepthPrepass`, `ReplaySelectionMask` are TODO stubs in `FrameDebuggerContext.cpp`. When implemented they should pull `capturedRenderFrameIndex % MAX_FRAMES_IN_FLIGHT` exactly like `ReplayGeometry`.
2. **Set 1 bindless / Sets 2 + 4 cycling.** Out of scope for this effort. Bindless has its own UploadContext pump; Material + BoneMatrix use game-stage tagging. They retain UAB, which is correct for their lifecycle.
3. **`renderer/` folder coherence drive-by** (carried from v2.9.12 follow-ups). Empty `gpu/` and `postprocess/` folders, lone `passes/ImGuiPass.cpp`. Cleanup deferred.
