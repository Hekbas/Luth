# rt-renderer.B.3 — rt-shadows

**Date:** 2026-05-31 (code landed on `main`) · wrap-up written 2026-06-06
**Commits:** 16 (on `feat/rt-shadows`, spanning 2026-05-26 → 2026-05-31)
**Issue:** [#140](https://github.com/Hekbas/Luth/issues/140)
**Umbrella:** [#127](https://github.com/Hekbas/Luth/issues/127)
**Series:** `rt-renderer`, Phase B.3 — closes Phase B. Mode A series-coalesced. Code shipped under the **v3.0.10** tag, *sharing* that tag with `gpu-debug-toolkit` (see below). Tag-only, no Release.

---

## Why this history is retroactive

The rt-shadows feature code landed on `feat/rt-shadows` and merged to `main` on 2026-05-31, but its formal wrap-up (this file, issue close, ROADMAP rows) was deferred and is completed here as part of the Phase B closeout.

A recurring skinned-character `VK_ERROR_DEVICE_LOST` surfaced during bring-up. The diagnostic scaffolding assembled to chase it grew into its own effort — `gpu-debug-toolkit` ([#143](https://github.com/Hekbas/Luth/issues/143)) — which took the `v3.0.10` tag *name* plus its own history file while rt-shadows rode along in the same merge commit. The device-lost root cause itself (a 4 MiB `BoneMatrixBuffer` taking the tagged-heap large-one-shot destroy path) was only nailed two efforts later in `gpu-device-lost` (v3.0.11). So the `feat/rt-shadows` branch carried three distinct things: the rt-shadows feature (this file), the gpu-debug-toolkit (its own file), and an early not-yet-root-caused patch of the bone-buffer UAF (`82a1d63`, properly fixed in `gpu-device-lost`).

---

## Overview

First **production consumer** of the RT pipeline stood up across B.1/B.2. Adds hardware-ray-traced sun shadows as the default directional-shadow path, with raster CSM retained as a runtime per-light compare toggle (`ShadowingMode`) — mirroring the existing `TonemapOperator` A/B precedent rather than retiring CSM outright. Scope is the **directional sun only**, **hard shadows**, **1 sample/pixel**; point/spot RT shadows and soft sun-disk + denoise are deferred to Phase C (ReSTIR DI + SVGF).

Pass shape: a dedicated `RtSunShadowsPass` on `QueueFamily::AsyncCompute` runs `rt_sun_shadows.rgen` (1-spp visibility ray, Wächter-Binder origin bias) + `rt_sun_shadows.rmiss` against the per-frame TLAS from B.2, writing a per-view R8 visibility mask. `pbr.frag` samples that mask (Set 3 binding 4) when `ShadowingMode::RtShadows` is active, otherwise runs the bit-identical CSM path. Exactly one mode's passes register per frame: in RT mode the CSM cascade build + the 4× cull/ShadowPass loops are skipped; in CSM mode the RT pass + the whole TLAS-build chain (skinning + BLAS refit + TLAS rebuild) is skipped.

A dedicated AsyncCompute pass writing an R8 mask was chosen over inline ray-query in `pbr.frag` for three reasons: AAA precedent (UE5/HDRP/Cyberpunk all decouple the visibility pass), forward-compatibility with the Phase C.1 ReSTIR DI / C.2 SVGF refactor (which expect a separate visibility/denoise buffer), and to exercise B.1's `VKRayTracingPipeline` + `RtShaderBindingTable` primitives in a real pipeline.

---

## Sub-tasks

| # | What landed | Commit |
|---|---|---|
| A | **`ShadowingMode` toggle + UBO field + persistent empty TLAS.** New `ShadowingMode` enum (`RasterCSM` / `RtShadows`, default `RtShadows`) + `RtOriginEpsilon`/`RtNormalEpsilon` on `Component::DirectionalLight`; `DirectionalLightSnapshot` / `LightGatherer` / `SceneSerializer` mirror the new fields. `GlobalUniforms` gains `rtShadowParams` Vec4 (mode, originEps, normalEps, pad) + `globals.glsl`. `RtSubsystem` builds a 0-instance **persistent empty TLAS** at `Init` so Set 0 binding 6 is never null (the raygen reads it statically); kept in a separate field from the per-frame `m_LastResult.tlas` so B.2's hash-skip `PushDeletion` can never destroy it. | `307699d` |
| B | **RT shadow pipeline + raygen/miss + R8 mask + AsyncCompute pass.** `rt_sun_shadows.rgen` (1-spp visibility ray, Wächter-Binder origin bias) + `rt_sun_shadows.rmiss` (`visibility = 1.0` on full-distance miss). `VKRayTracingPipeline` + `RtShaderBindingTable` instantiated in `RtSubsystem` (first production use of B.1 primitives); pipeline layout `[Global, Light @ Set 1, ShadowPass]`. Per-view R8 `sunShadowMask` on `ViewResources` (mirrors `taaHistoryA/B` lifetime); cycled `rtShadowPassDescSet[MAX_FRAMES_IN_FLIGHT]`. `AddRtSunShadowsPass` imports the mask + declares `WriteStorageImage`; inline AS-build → ray-tracing-read barrier covers TLAS visibility to the raygen. RG `ComputeRead`/`ComputeWrite` stage masks widened with `RAY_TRACING_SHADER_BIT_KHR`. | `27aa5c1` |
| C | **Set 3 binding 4 consumer + `pbr.frag` dispatch.** `LightingSubsystem` Set 3 grows 4 → 5 bindings; binding 4 = sun-shadow-mask sampler (linear clamp-to-edge), written per view in `WriteShadowView` alongside the cascade sampler. `pbr.frag`: `ComputeShadow` → `ComputeShadowCSM` (verbatim move, CSM path bit-identical) + new `ComputeShadowRT` sampling `sunShadowMask` at `gl_FragCoord/viewportSize`; top-level `ComputeShadow` dispatches on `ubo.rtShadowParams.x`; the `shadowBias.x < 0` no-shadow sentinel is preserved. | `50aab2f` |
| — | **TLAS pass plumbing + consumer chain.** RG `hasSideEffect` so `TlasBuildPass` survives dead-pass culling + `RAY_TRACING` stage in `Compute{Read,Write}`. `sceneDepth` + `slimNormal` threaded into `AddRtSunShadowsPass` via `ReadStorageImage`. TLAS bound to Set 0 b6 in `GlobalSubsystem::UpdateUBO`; mask sampler masked to Set 3 b4. Cascade-map layout initialized to `SHADER_READ_ONLY_OPTIMAL` so RT mode matches the descriptor write. `.meta` files for `rt_smoke` + `rt_sun_shadows`. | `c16ad92` |
| D | **Mode gating + skip-when-inactive.** `LightingSystem::UpdateFor` skips `CascadeBuilder::Build` when `RtShadows`. `RenderPipeline::Execute` gates the 4× cull-cascade + 4× `AddShadowPass` loops on `(RasterCSM && castShadows)`; `AddRtSunShadowsPass` on `(RtShadows && castShadows)`. `GeometrySubsystem` already no-ops an invalid mask handle. Result: exactly one mode's passes register per frame; toggle round-trips with no leftover passes. Plus `perf`: the entire skinning + BLAS-refit + TLAS-build chain is skipped outside RT mode (CSM has no TLAS consumer). | `ff9ded7`, `9784a72` |
| E | **Editor UI + JSON copy/paste.** `DirectionalLightDrawer`: `UI::PropertyCombo` for `Shadowing` (Raster CSM / RT Shadows), mirroring `RenderPanel`'s `TonemapOperator` pattern. CSM branch keeps the existing bias/normal/blend/size/distance sliders; RT branch exposes `RtOriginEpsilon` (1e-4..1e-2) + `RtNormalEpsilon` (1e-3..0.5). `DebugVisualizeCascades` stays exposed in both modes. JSON copy/paste round-trips `Shadowing` + RT epsilons across entity duplication. | `15b89ba` |
| F | **Wrap-up (this file).** Retroactive — see "Why this history is retroactive". ROADMAP completed-table rows for v3.0.8–v3.0.11; Phase B table marked done; B.3 "CSM retires" reconciled to "CSM-as-toggle" in ROADMAP + umbrella #127. Issue #140 closed. `Version.h` already at 3.0.11 (bumped by later efforts). | — |

---

## Architectural decisions

### Default RT, CSM-as-toggle — *deviation* from the umbrella's "CSM retires"

The umbrella ([#127](https://github.com/Hekbas/Luth/issues/127)), ROADMAP, and the arc spec all originally said B.3 *retires* raster CSM ("RT-mandatory, no raster fallback"). Shipped instead with CSM **retained** as an opt-in `ShadowingMode` toggle. Rationale, decided during planning and recorded in #140's body: the side-by-side A/B (RT vs CSM on the same sun) is a portfolio asset, costs one enum + one shader branch, and mirrors the established `TonemapOperator` compare precedent. The **hardware** requirement stays RT-mandatory — B.1 gates device selection on the four RT extensions, so there is no non-RT build; only the CSM *code path* is kept. ROADMAP B.3 row + umbrella updated to match this reality.

### RT pipeline descriptor-set remapping

The PBR pipeline binds the Light descriptor set at Set 3; the RT shadow pipeline's layout is `[Global=0, Light=1, ShadowPass=2]`. The same `VkDescriptorSet` handle binds at different set indices per pipeline — this is a per-pipeline-layout remap, not a global Set reshuffle, so nothing in the raster path moves.

### Per-view R8 mask lifetime mirrors TAA history

`sunShadowMask` is allocated/resized on `ViewResources` exactly like `taaHistoryA/B` (per view, recreated on resize via `EnsureViewResources`). RT shadow tracing is **per-view** (each view's depth/camera differ), so unlike the scene-global TLAS build there is no multi-view short-circuit on the shadow pass.

### Volumetric god rays still need CSM cascades even in RT mode

`volumetric_inject_scatter.comp` samples the cascade shadow map at Set 1 b5 unconditionally for sun-shielded in-scattering. So cascades are built whenever `castShadows` is on (not only in `RasterCSM` mode), and the cull-cascade + `ShadowPass` loops run when `(RasterCSM || view.enableVolumetricFog)`. Without this, god rays stopped tracking sun rotation in RT mode. Folding volumetric self-shadowing onto RT shadow rays is Phase D.2 (`volumetric-rt-shadows`).

---

## Bugs caught during bring-up

- **Missing triangle hit group → TDR** (`c51eade`). The RT shadow pipeline shipped raygen + miss but no hit group; `vkCmdTraceRaysKHR` with an empty hit region drove a device hang. Fix: add a `TRIANGLES_HIT_GROUP` with `VK_SHADER_UNUSED_KHR` shaders (shadow rays only need the miss/no-miss distinction), bump `sbt.counts.hitCount` to 1, pass `sbt.GetHitRegion()`.
- **AS storage sharing + scratch heap** (`e4ab013`). Static + skinned BLAS storage and the per-frame/persistent TLAS storage are read cross-queue by the RT raygen → `ApplyConcurrentSharing`. Per-frame AS-build scratch moved off the HOST_VISIBLE tagged heap to plain VMA `DEVICE_LOCAL`; per-build `BuildCtx` heap-allocated so geometry/range structs outlive the build stack frame.
- **`BuildEmptyTlas` dropped its `VmaAllocation`** (`4b5bfae`). The 0-instance TLAS scratch alloc ignored `AllocateBuffer`'s return value, leaving the handle null; the matching `FreeBuffer(buf, null)` then crashed inside VMA at engine init — surfacing as a spurious access violation rerouted through the ImGui focus callback.
- **Bone matrix buffer use-after-free** (`82a1d63`, *partial*). First attempt at the skinned-vertex page-fault: tag the per-frame bone region `gameFrame+1` so retirement trails GPU consumption. Reduced but did not eliminate the `DEVICE_LOST`; the true root cause (4 MiB buffer on the tagged-heap large-one-shot **destroy** path) was found in `gpu-device-lost` (v3.0.11).
- **Skinned AS input init + sharing** (`186b8df`), **refit scratch lifetime** (`4df8504`), **cross-frame compute wait guards refit** (`fea1d49`) — barrier/lifetime hardening on the per-frame skinned-BLAS refit so frame K's refit can't overlap frame K-1's `traceRays` on the same BLAS.
- **RT shader extensions not recognized** (`dcddbe3`). `.rgen/.rmiss/.rchit/...` weren't mapped to the `Shader` asset type, so the importer skipped them. Registered the six RT extensions.

---

## Files touched

**Engine (Luth.lib) — new**
- [`rt_sun_shadows.rgen`](../../../luth/assets/shaders/rt_sun_shadows.rgen) / [`rt_sun_shadows.rmiss`](../../../luth/assets/shaders/rt_sun_shadows.rmiss) — 1-spp sun visibility raygen + miss

**Modified**
- [`RtSubsystem.{h,cpp}`](../../../luth/source/luth/renderer/subsystems/RtSubsystem.h) — persistent empty TLAS, RT shadow pipeline + SBT, `AddRtSunShadowsPass`, `WriteShadowPassView`
- [`Lights.h`](../../../luth/source/luth/scene/components/Lights.h) + [`LightTypes.h`](../../../luth/source/luth/renderer/lighting/LightTypes.h) — `ShadowingMode` enum + RT epsilons on `DirectionalLight`
- [`LightingSubsystem.cpp`](../../../luth/source/luth/renderer/subsystems/LightingSubsystem.cpp) — Set 3 4→5 bindings, b4 mask, mode-gated cascade build
- [`GeometrySubsystem.cpp`](../../../luth/source/luth/renderer/subsystems/GeometrySubsystem.cpp) + [`GlobalSubsystem.cpp`](../../../luth/source/luth/renderer/subsystems/GlobalSubsystem.cpp) — mask Read/transition, TLAS Set 0 b6
- [`RenderPipeline.cpp`](../../../luth/source/luth/renderer/RenderPipeline.cpp) — per-mode pass gating, mask handle threading
- [`ViewResources.cpp`](../../../luth/source/luth/renderer/ViewResources.cpp) + [`RenderSnapshot.h`](../../../luth/source/luth/core/RenderSnapshot.h) — per-view R8 mask + pass-local desc sets, snapshot `ShadowingMode`
- [`RenderGraph.cpp`](../../../luth/source/luth/renderer/rendergraph/RenderGraph.cpp) — `RAY_TRACING` stage in Compute{Read,Write}, `hasSideEffect`
- [`SceneSerializer.cpp`](../../../luth/source/luth/scene/SceneSerializer.cpp) + [`LightGatherer`](../../../luth/source/luth/renderer/lighting/) — persist/mirror new fields
- [`VulkanAccelerationStructure.cpp`](../../../luth/source/luth/renderer/backend/vulkan/VulkanAccelerationStructure.cpp) — empty-TLAS alloc fix, CONCURRENT sharing
- [`volumetric_inject_scatter.comp`](../../../luth/assets/shaders/volumetric_inject_scatter.comp) — sample CSM cascades regardless of shadow mode

**Shaders** — [`pbr.frag`](../../../luth/assets/shaders/pbr.frag) (CSM/RT dispatch), [`globals.glsl`](../../../luth/assets/shaders/common/globals.glsl) (`rtShadowParams`)

**Editor (Luthien.lib)** — [`DirectionalLightDrawer.cpp`](../../../luthien/source/luthien/inspectors/component_drawers/DirectionalLightDrawer.cpp) — Shadowing combo + RT sliders + JSON

---

## Verification

Build clean (Debug + Release). Editor boots; RT-mandatory device check passes. RT shadows visible by default on Bhaal Temple / `character_test`; the inspector `Shadowing` combo flips between RT and Raster CSM live with no leftover passes (RenderDoc shows the alternate mode's passes absent). God rays track sun rotation in both modes (cascade-build-in-RT-mode fix). JSON copy/paste of a DirectionalLight round-trips `Shadowing` + epsilons.

**Known follow-on at landing time:** the recurring skinned `VK_ERROR_DEVICE_LOST` (chased here, partially patched in `82a1d63`) was not fully resolved until `gpu-device-lost` (v3.0.11) — see that history file for the root cause + structural fix.

---

## Deviations from issue #140

- **Point-light RT shadows descoped.** #140 originally listed sun + point; shipped sun-only. Point/spot RT shadows fold into Phase C.1 ReSTIR DI (one visibility ray covers light selection + shadow). No separate effort.
- **Wrap-up sub-task F deferred** then completed retroactively (this file), for the reasons in the header.
