# rt-renderer.C.1 — restir-di

**Date:** 2026-06-07
**Commits:** 7 on `feat/restir-di` (S0 `b01b7db`, S1 `e0c64e7`, S2 `45ecb4a`, M-cap fix `5b95c96`, S3 `2908545`, S4 `3f935b4`, wrap-up)
**Issue:** [#146](https://github.com/Hekbas/Luth/issues/146)
**Umbrella:** [#127](https://github.com/Hekbas/Luth/issues/127)
**Series:** `rt-renderer`, Phase C.1 — opens Phase C (RT global illumination). Mode A series-coalesced, **v3.0.12** tag-only, no Release.

---

## Overview

First ReSTIR effort: Bitterli 2020 spatiotemporal reservoir resampling for **direct lighting**, giving the engine's point lights shadowed direct lighting for the first time at a bounded ~1 ray/pixel. Before this, the Forward+ point-light loop in `pbr.frag` was fully **unshadowed**; only the sun had an RT shadow mask (B.3). ReSTIR DI replaces that loop with a per-pixel reservoir pipeline that selects one light via RIS, tests its visibility with an inline ray query, reuses reservoirs across frames (temporal) and neighbours (spatial), and writes a **demodulated** diffuse-irradiance image that `pbr.frag` remodulates.

Pass shape (all compute on `QueueFamily::AsyncCompute`, gated on the graphics-queue G-buffer like B.3): **initial** (RIS over the flat light list + one `rayQueryEXT` visibility ray) → **temporal** (motion-vector reprojection + confidence-weighted combine with last frame's reservoir) → **spatial** (k-neighbour disk merge) → **shade** (demodulated `E = Li·NdotL·W`). `pbr.frag` Set 3 b5 samples the DI image and remodulates by `albedo·(1-metallic)/PI`, gated on `restirParams.x`; the unshadowed cluster loop is kept as the A/B fallback.

**Justification** (recorded at planning time): not the paper's million-light speedups — those don't apply at the engine's tens-of-lights regime and were explicitly refuted in the deep-research pass. The real value here is bounded-cost shadowing of the previously-unshadowed point lights, denoiser-friendly temporal stability, and standing up the reservoir/GRIS machinery that C.2 (SVGF) and C.3 (ReSTIR GI) build on. Portfolio/Larian-Framework-5 alignment is the strategic driver.

---

## Sub-tasks

| # | What landed | Commit |
|---|---|---|
| S0 | **Device-local (Garlic) tagged allocation path.** The crux finding: `GPUTaggedPageAllocator` is `HOST_VISIBLE`-only (the Onion half) — reservoirs are GPU-only read+write and would saturate PCIe there. Added `AllocateLargeTaggedDeviceLocal` — a `DEVICE_LOCAL`, non-mapped sibling of `AllocateLargeTagged` with a disjoint recycle pool (`isDeviceLocal` flag), branched `FreeTag`/`Shutdown`/`FlushRegion`/`GetStats`. Reuses the existing `VulkanAllocator::AllocateBuffer(.., AUTO_PREFER_DEVICE)` rather than a new wrapper. Advances the "GPU side under construction" of the ND Onion/Garlic model. | `b01b7db` |
| S1 | **Reservoirs + Set 2 + initial RIS + visibility + shade + forward consumption.** New `RtRestirSubsystem` (2 compute pipelines), `restir_common.glsl` (32 B `Reservoir` + WRS/RIS/PCG/oct helpers), `restir_initial.comp` (RIS over M candidates + one rayQuery on the selected sample) + `restir_shade.comp` (demodulated diffuse irradiance). Pass-local Set 2; per-view DI image (rgba16f) + a single device-local reservoir buffer. `pbr.frag` point loop → DI read gated on a new `restirParams` UBO field; Set 3 grows to b5. First `rayQueryEXT` usage in the engine. | `e0c64e7` |
| S2 | **Temporal reuse.** `restir_temporal.comp` between initial and shade. Reservoir becomes a **ping-pong pair** (parity-swapped b2/b4, UAB). Motion-vector reprojection (TAA convention) + history validation (linear depth + normal) + confidence-weighted combine. The reservoir **self-carries its pixel geometry in the `_pad` fields** (raw depth + oct normal), so validation reads the previous reservoir directly — no previous-frame G-buffer textures and no copy pass. | `45ecb4a` |
| — | **Relative M-cap fix.** Temporal was under-accumulating: an absolute M-cap of 20 lost to the initial reservoir's M=32 (candidate count). Changed to the Bitterli relative clamp `prev.M ≤ mCap × curr.M`, so history accumulates (~640) and dominates the fresh sample. This is what killed the lit-area "ant crawling". | `5b95c96` |
| S3 | **Spatial reuse.** `restir_spatial.comp` between temporal and shade. k-neighbour disk merge with geometric (depth/normal) rejection, into a separate single device-local output buffer (neighbours must be read un-modified — never in-place); the temporal buffer stays intact as history. **No Jacobian** — punctual lights store a light index, so the shift map is identity and re-evaluating the target at the current pixel is exact (the Jacobian is an area-light / GI concern, reserved for later via the `_pad` slots). Shade rebinds to read b6. | `2908545` |
| S4 | **Editor tuning controls.** `RestirSettings` (enabled, candidate count, temporal M-cap + thresholds, spatial neighbours/radius/threshold) on `RenderingSystem`, mirroring `VolumetricSettings`. Subsystem reads it (replaces the hardcoded `k_*` constants); `enabled` backs `IsEnabled()` and the `restirParams.x` gate. New "ReSTIR DI" section in the editor `RenderPanel` post-process tab. | `3f935b4` |

---

## Architectural decisions

### Device-local (Garlic) allocator extension — *refinement* of the spec's "reservoirs → tagged heap"

The arc spec locks per-frame GPU buffers (incl. reservoirs) to `GPUTaggedPageAllocator`. Investigation found that heap is `HOST_VISIBLE | MAPPED` only — the Onion half, built for CPU-write/GPU-read upload SSBOs. Reservoirs are Garlic-class (GPU-only read+write, ~0.8 GB/frame across the four passes). The spec's *intent* (the tagged lifetime model — bulk-free, recycle, no fixed-pool ring buffers) is honoured; the *letter* (host-visible) would be a PCIe trap on discrete GPUs. So S0 added the device-local tagged path the GPU-side model was missing. Reservoirs use it as **persistent per-view allocations** (reserved high-range tags, freed only on resize — never the per-frame `FreeTag(N-2)` sweep), not per-frame-tag-churned.

### rayQuery-in-compute, not SBT

`VK_KHR_ray_query` was enabled in B.1 but never used (all RT was SBT raygen). ReSTIR's passes are compute resampling kernels that trace inline at the selected sample, so `rayQueryEXT` in compute is the natural fit and avoids SBT churn across the multi-pass chain. **The AS-build→AS-read barrier uses `dstStageMask = COMPUTE_SHADER_BIT`, not the `RAY_TRACING_SHADER_BIT` the B.3 SBT pass uses** — copying that barrier verbatim would read a still-building TLAS (a TDR trap, called out explicitly in planning).

### Reservoir self-carries geometry — no previous-frame G-buffer

Temporal validation needs the previous frame's depth+normal at the reprojected pixel. Rather than keep prev-frame G-buffer textures + a copy pass, the 32 B reservoir stores its origin pixel's raw depth + oct normal in its three `_pad` floats. The temporal pass reads the previous reservoir's pads directly. This eliminated an entire texture set + copy pass from the S2 wiring. (The pads were originally reserved for a future area-light point-on-light; that forward-compat is deferred — when ReSTIR GI / area lights land, the struct grows.)

### Demodulated diffuse-only output

`restir_shade.comp` writes `E = Li·NdotL·W` (no albedo, no 1/π); `pbr.frag` remodulates `E·albedo·(1-metallic)/π`. This keeps the signal albedo-free so C.2 SVGF can denoise it directly. Point-light **specular** is dropped for v1 (small for the torch use-case); a clean diffuse/specular split lands when C.2 needs a separate specular channel.

### Sun stays separate; biased combine

The sun keeps its B.3 RT mask path — ReSTIR owns the point lights only (a single dominant light competing in the reservoir would add sun-shadow noise; NRD's separate SIGMA denoiser is the industry precedent). The temporal and spatial combines are the **biased** confidence-weighted variant (no per-sample MIS denominators) — the standard practical first cut; the unbiased generalized-balance-heuristic refinement is a later option if the slight edge bias matters.

---

## Bugs caught during bring-up

- **Metals had non-zero diffuse** (caught in S1 review). The demodulation remodulated `E·albedo/π`, which gives metals a Lambertian diffuse they shouldn't have. Fixed to `E·albedo·(1-metallic)/π` (diffuse albedo). My spec's error, not the implementation's.
- **Temporal under-accumulation** (`5b95c96`). Absolute M-cap of 20 < the initial reservoir's M=32, so the fresh noisy sample out-weighted accumulated history every frame → persistent lit-area flicker. Fixed to the Bitterli relative clamp (`prev.M ≤ mCap × curr.M`). This was the main contributor to the user-observed "ants".
- **Build wrappers hang on `pause`** (process, not code). `scripts/*/{setup,build}_windows.bat` use `pushd ..\..` relative to their own folder + a trailing `pause`; invoked from the repo root they resolved two levels too high and blocked on the keystroke prompt. Worked around by invoking premake + MSBuild directly from the repo root.

---

## Files touched

**Engine (Luth.lib) — new**
- [`RtRestirSubsystem.{h,cpp}`](../../../luth/source/luth/renderer/subsystems/RtRestirSubsystem.h) — 4 compute pipelines, Set 2 pass-local layout, reservoir ping-pong + spatial buffer + DI image lifecycle, the 4-pass `AddPasses` chain, per-frame ping-pong binding
- [`RestirSettings.h`](../../../luth/source/luth/renderer/settings/RestirSettings.h) — tunable knobs
- Shaders: [`restir_common.glsl`](../../../luth/assets/shaders/common/restir_common.glsl), [`restir_initial.comp`](../../../luth/assets/shaders/restir_initial.comp), [`restir_temporal.comp`](../../../luth/assets/shaders/restir_temporal.comp), [`restir_spatial.comp`](../../../luth/assets/shaders/restir_spatial.comp), [`restir_shade.comp`](../../../luth/assets/shaders/restir_shade.comp) (+ `.meta`)

**Engine — modified**
- [`GPUTaggedPageAllocator.{h,cpp}`](../../../luth/source/luth/memory/GPUTaggedPageAllocator.h) — device-local large-tagged path
- [`RenderPipeline.{h,cpp}`](../../../luth/source/luth/renderer/RenderPipeline.h) — `m_Restir` subsystem, ViewResources reservoir/spatial/DI fields, pass wiring
- [`ViewResources.cpp`](../../../luth/source/luth/renderer/ViewResources.cpp) — device-local buffer alloc/free + pool bumps + WriteView calls
- [`LightingSubsystem.cpp`](../../../luth/source/luth/renderer/subsystems/LightingSubsystem.cpp) — Set 3 b5 DI sampler + COMPUTE on b0
- [`GeometrySubsystem.{h,cpp}`](../../../luth/source/luth/renderer/subsystems/GeometrySubsystem.h) — DI handle Read/thread
- [`GlobalSubsystem.cpp`](../../../luth/source/luth/renderer/subsystems/GlobalSubsystem.cpp) — `restirParams.x` fill
- [`RenderingSystem.h`](../../../luth/source/luth/scene/systems/RenderingSystem.h) — `restirParams` UBO field + `RestirSettings` member
- Shaders: [`pbr.frag`](../../../luth/assets/shaders/pbr.frag) (DI read + dual path), [`globals.glsl`](../../../luth/assets/shaders/common/globals.glsl) (`restirParams`)
- [`arch/memory.md`](../../arch/memory.md) — Onion/Garlic device-local note

**Editor (Luthien.lib)** — [`RenderPanel.cpp`](../../../luthien/source/luthien/panels/RenderPanel.cpp) — ReSTIR DI tuning section

---

## Verification

Build clean (Debug x64, luth → luthien → Runtime), no new warnings. All four ReSTIR compute shaders pass `glslc --target-env=vulkan1.3`. Smoke-tested per sub-task on a point-light scene: S1 — point lights cast contact shadows (noisy, 1 spp, as expected); S2 — noise drops, converges when still; S3 — lit-area noise gone, residual only in penumbras (SVGF territory); S4 — editor controls live, on/off toggle A/Bs cleanly. Cross-frame reservoir read relies on frame-pipeline ordering (TAA-history shape) — no sync-validation issues observed.

---

## Hand-off to C.2 (SVGF) and deviations

**C.2 slot-in.** The demodulated DI image (`restir_shade.comp` output) is the SVGF input. SVGF inserts between `RestirShade` and `GeometryPass`: denoise the demodulated `E` → `pbr.frag` remodulates the *denoised* signal (the `albedo·(1-metallic)/π` line is unchanged). The penumbra noise is the intended SVGF target. Three temporal loops then coexist — ReSTIR reuse (sample domain), SVGF (radiance domain), TAA (image domain, AA-only); keep them separated per the deep-research findings.

**Deviations from the plan.** (1) Reservoir self-carries geometry in pads — dropped the planned prev-G-buffer textures. (2) S1 kept the two-pass split (initial→shade) to exercise S0; visibility stays in initial through S2/S3 (reused samples carry prior visibility, M-cap-bounded) rather than a per-pass re-test. (3) No Jacobian (punctual lights). (4) Biased combine, not unbiased balance-heuristic. (5) Editor toggle (planned for S4) doubles as the A/B the plan deferred. (6) Git hooks are not installed in this workspace (`scripts/git-hooks` empty) — comment policy honoured manually.
