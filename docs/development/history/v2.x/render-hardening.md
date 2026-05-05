# v2.9.11 — render-hardening

**Date:** 2026-05-06
**Commits:** 9 (on `fix/render-hardening`)
**Issue:** [#120](https://github.com/Hekbas/Luth/issues/120)

---

## Overview

Batch of correctness fixes targeting the per-frame Vulkan render path. Originally scoped narrowly as "per-frame UBO race"; expanded after audit-driven review surfaced additional latent issues in tagged-heap reclamation timing, multi-view descriptor sharing, and cross-view resource synchronization. None of the fixes resolve the user-reported "motion-artifact / black afterimage during motion" symptom — that bug is older than v2.8 and remains under separate investigation. The fixes shipped here are architecturally correct on their own merits.

The work landed as a single multi-effort branch with no intermediate tags (CI/release noise concern). Discipline: no speculative commits — every change is backed by either Vulkan spec citation, the engine's existing `gpu-tagged-heap` pattern, or an audit finding.

---

## Per-frame UBO migration to `GPUTaggedPageAllocator` (commits A–F)

Pre-existing hazard: per-frame UBOs (Set 0 GlobalUniforms+GTAO, Set 3 LightUniforms, 4 PostProcess sets, Grid set, GTAO main set) were single-instance `VKUniformBuffer`s. CPU `memcpy`'d new view/proj/lights/etc into them every frame while previously-submitted command buffers might still be on the GPU. Vulkan permits this only if descriptor + pool flags allow `UPDATE_AFTER_BIND` and the buffer storage is properly versioned — the prior code had neither.

The fix replicates the SSBO migration pattern from v2.8.10 `gpu-tagged-heap` for UBOs:

| # | Commit | Sub-task |
|---|--------|----------|
| A | `f6b47dd` | Enable `descriptorBindingUniformBufferUpdateAfterBind` device feature; add `VulkanContext::GetMinUniformBufferAlignment()` accessor |
| B | `155941a` | Layout/pool flag retrofit: `UPDATE_AFTER_BIND_BIT` + `UPDATE_AFTER_BIND_POOL_BIT` across Set 0 (bindings 0+5), Set 3 (binding 0), PP layout (binding 2), Grid (binding 0), GTAO main (binding 2) |
| C | `beef848` | Migrate Light UBO (Set 3 binding 0): `UploadLightUBO` allocates per-frame region, rewrites descriptor; drop `m_LightUniformBuffer` member + initial Set 3 binding 0 write |
| D | `00eaa66` | Migrate Global + GTAO UBOs (Set 0 bindings 0/5 + Grid binding 0 + GTAO main binding 2): hoisted descriptor writes per render-stage entry into the existing `Update*` path |
| E | `020cfd9` | Migrate PostProcess UBO (4 PP descriptor sets binding 2) |
| F | `5d6769c` | GPU heap backings gain `VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT` |

Sub-task F was strictly required after the migration: without the usage flag, `vkUpdateDescriptorSets` with `descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER` pointing at heap-backed memory hit `VUID-VkWriteDescriptorSet-descriptorType-00330`. Surfaced as a validation error during smoke testing of sub-task E.

The per-write-site shape matches the SSBO template from `MaterialSystem::Update` and `GPUObjectBuffers::BuildGPUObjectBuffer`: set tag = render-frame index, allocate from `GPUTaggedPageAllocator`, `memcpy` UBO into mapped region, `FlushRegion` (no-op on host-coherent), rewrite descriptor with `vkUpdateDescriptorSets`. Pages are reclaimed by `FreeTag` once the GPU retires the consuming submission.

---

## FreeTag math (commit G `d0cd830`)

Pre-existing bug in v2.8.10's tagged-heap driver, surfaced when audit traced the page lifecycle.

`VulkanBackend::AcquireImage` was calling `FreeTag(waitValue)` after the timeline wait. Tag T is referenced by **two** submissions:

- Iter T's command buffer (game stage allocates with `GetFrameIndex()` = T) — GPU-done at timeline T+1.
- Iter T+1's command buffer (render stage allocates with `GetRenderFrameIndex()` = T) — GPU-done at timeline T+2.

Tag T is therefore safe to free only after timeline T+2 retires. `waitValue` confirms timeline V = iter (V-1) done; safe-to-free is `T ≤ V - 2`. Old code freed too aggressively (`T = V`), recycling pages while in-flight cmd buffers still read them. Pages were re-allocated by the next frame's CPU writes and partially overwritten — a classic data race that the existing test scenes failed to expose because identical-frame-to-frame data masks the corruption.

Fix: shift the FreeTag value by 2 (`finishedTag = waitValue - 2`) and guard the `waitValue >= 2` boundary case for the first 4 frames after warm-up.

The tag scheme stays asymmetric (game `GetFrameIndex`, render `GetRenderFrameIndex`) — unifying it would be a follow-up cleanup but doesn't change correctness with the corrected math.

---

## Set 3 multi-view race (commit H `0348e56`)

Latent bug found during audit. Set 3 (Light UBO + shadow sampler) is a single global descriptor — there's only one `m_LightDescSet`, shared by all views. `UploadLightUBO` was called inside `RecordView` once per view, rewriting Set 3 binding 0 each call.

With `UPDATE_AFTER_BIND` late-write semantics, the descriptor seen by *all* in-flight draws referencing Set 3 reflects the **most recent** update at submit time. View1's GeometryPass draws bind Set 3 → record → View2's `UploadLightUBO` rewrites Set 3 binding 0 → both views' draws now read View2's region. Today invisible because lights are gathered from snapshot (camera-independent); becomes visible the moment per-view lighting is added.

Fix: hoist `UpdateFor` + `UploadLightUBO` once per `Update` before the view loop. `m_Lights` is view-independent; cascades stay per-view in `RecordView` (route via Set 0, which is per-view).

---

## Cross-view shadow map sync (commit I `24865fb`)

Latent bug found during audit. `m_ShadowMap` is a single physical D32 array texture shared across view subgraphs. Each view's render graph imports it as `RG::ResourceState::Undefined` (4 cascade imports). The Undefined import discards content and inserts `srcStage=TOP_OF_PIPE, srcAccess=0` barrier — no execution dependency on prior consumers.

Within one view, this is fine: barriers chain correctly internally (Undefined → DepthAttachment for ShadowPass, DepthAttachment → ShaderRead before GeometryPass). **Cross-view there is no RAW dependency** between View1's GeometryPass shader-read and View2's ShadowPass depth-write. The driver may overlap V2's depth writes with V1's still-in-flight fragment shader sampling — undefined GPU behavior. Visible as intermittent shadow corruption when both Scene and Game viewports are open.

Fix: `RenderingSystem::InsertInterViewBarrier` emits a `VkMemoryBarrier2` with `srcStage=FRAGMENT_SHADER, srcAccess=SHADER_READ → dstStage=EARLY_FRAGMENT_TESTS, dstAccess=DEPTH_STENCIL_ATTACHMENT_WRITE` on the primary cmd buffer between consecutive view subgraphs. Sufficient because `m_ShadowMap` is the only resource shared across views; per-view RTs (SceneColor, SceneDepth, EntityID, GTAO buffers) are owned by per-view `FrameTargets` / `ViewResources`.

A more architecturally clean fix would be RG-level: track resource post-state across `RG::RenderGraph` instances and import on subsequent views with the correct prior state (e.g., `ShaderResource`). That's a larger refactor; manual barrier is the pragmatic seam.

---

## Reverted speculative work

A camera-snapshot commit (`bf6d466`) was authored mid-effort under the hypothesis that "frame-N camera + frame-N-1 mesh worldMatrices = afterimage". Audit re-derivation showed: for static-scene + camera motion (the typical test case), the OLD code path `(camera-N, world-N-1)` was input-coherent — `world-N-1 == world-N` when meshes are static. The "fix" added one frame of camera input latency without fixing anything visible.

Dropped via `git reset --hard 5d6769c` + cherry-pick of legitimate commits — the branch was unpublished, so rewriting was strictly cleaner than carrying revert noise. No `bf6d466` or its revert appears in the final history.

---

## Architectural alignment

- **Cornerstone 1 (per-frame data through tagged allocators):** UBO migration replicates the existing `GPUTaggedPageAllocator` SSBO pattern verbatim. No new primitive.
- **Cornerstone 4 (no `new`/`delete` in render):** unchanged; all new allocations route through `LH_NEW`-equivalent macros via VMA.
- **Cornerstone 5 (no legacy Vulkan):** continues — UPDATE_AFTER_BIND is a Vulkan 1.2 core feature; `vkCmdPipelineBarrier2` is Vulkan 1.3 sync2.
- **`arch/rendering-pipeline.md` — descriptor set table:** Set 3's binding 0 lifetime now reads "per render stage — rebound to fresh tagged-heap region". Doc update folded into this commit.
- **`arch/memory.md` — V6 driver wiring:** `FreeTag` semantics now correctly described — the `waitValue - 2` math is invariant-documented at the call site.

---

## Outstanding follow-ups

1. **Motion-artifact (the original symptom).** Pre-v2.8 origin per the user; investigation continues in a fresh conversation. Bisect-first plan documented in the hand-off doc (private).
2. **Asymmetric tag scheme** (`GetFrameIndex` vs. `GetRenderFrameIndex`). Audit-3 called it "fragile" — a future contributor introducing a Game-stage allocator that uses `GetRenderFrameIndex()` would silently break the FreeTag invariant. Cleanup-only follow-up.
3. **Cross-view RG state tracking.** The inter-view barrier ships as a localized fix; an RG-level enhancement that imports `m_ShadowMap` with prior-frame post-state on subsequent views would generalize beyond the shadow map and eliminate the manual barrier. Out of scope for this effort.
4. **`TaggedPageAllocator` (CPU heap) is currently dead code** — `FreeTag` calls in `VulkanBackend::AcquireImage` are no-ops because no callsite sets `CpuCache.CurrentTag`. Defensive symmetry; document as such.

---

## Build verification

Debug x64 builds clean (0 errors, pre-existing warnings only). Validation layers clean across exercises:

- Drag editor camera 60+s in Lit / Unlit / Wireframe — no `VUID-vkUpdateDescriptorSets-None`.
- PostProcess settings toggled during motion — no descriptor mismatch errors.
- Game panel + Scene panel concurrent — shadow map renders correctly without inter-view artifacts.
- Long-session memory: heap working set steady-state ~6 MB; `GrowBackingPoolLocked` infrequent after warm-up.
