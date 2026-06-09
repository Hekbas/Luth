# rt-renderer.C.5 — path-trace-reference

**Date:** 2026-06-09
**Commits:** 7 on `feat/path-trace-reference` (S0 `7cabc4e`, S1 `9e6aa8e`, S2 `ae21337`, S3 `2e24094`, S4 `b200adb`, smoke fixes `8744f03` + `aad3b57`, wrap-up)
**Issue:** [#148](https://github.com/Hekbas/Luth/issues/148)
**Umbrella:** [#127](https://github.com/Hekbas/Luth/issues/127)
**Series:** `rt-renderer`, Phase C.5. Mode A series-coalesced, **v3.0.16** tag-only, no Release.

---

## Overview

A ground-truth **path-traced reference** render mode — a rayQuery-in-compute megakernel that reuses the
Phase B TLAS + Phase C bindless-material infra to brute-force a physically-correct image, progressively
accumulated across frames (reset on camera/scene change). It is the oracle that validates ReSTIR DI/GI
convergence, and the portfolio's "real-time RT vs ground-truth PT" side-by-side.

A top-level `RenderMode::PathTrace` toggle (distinct from the `ShadeMode` debug-blit enum) swaps the whole
raster + ReSTIR chain for the megakernel: it traces its OWN jittered primary camera rays (no G-buffer
dependency → free progressive AA), walks a multi-bounce NEE path with the full Cook-Torrance BRDF +
Russian roulette, and accumulates an fp32 running mean. The output replaces `sceneColor` ahead of the
existing bloom + tonemap chain. Full PBR (per the locked decision), so the reference includes specular +
multi-bounce the real-time path lacks until Phase D.

Two adversarial-review workflows (S1 sync/accumulation, S3 BRDF/VNDF/MIS) ran against the real code and
confirmed the cross-frame barrier, in-place RMW, AS-barrier, running-mean, NEE, and the VNDF/lobe-MIS
math; both surfaced only non-bugs or already-applied fixes.

---

## Sub-tasks

| # | What landed | Commit |
|---|---|---|
| S0 | **Subsystem scaffold + RenderMode seam.** New `PathTraceSubsystem` (1 compute pipeline, pass-local Set 2 = b0 fp32 accumulator + b1 fp16 display), `RenderMode` enum + `PathTraceSettings` + `pathTraceParams` UBO, per-view `ptAccum`(rgba32f) + `ptColor`(rgba16f) + bootstrap clear + pool bumps, `BuildGraph` raster-bypass (HDR → bloom/composite, TAA + overlays gated off), `UpdateBloomCompositeInput` PT branch, minimal editor toggle. Stub test-pattern shader. | `7cabc4e` |
| S1 | **Primary rays + direct NEE + accumulation + reset.** Jittered primary camera rays, commit-hit, shared `common/geom_table.glsl` material deref (`restir_gi_initial.comp` routed through it), point+sun NEE, emission, IBL prefiltered env on miss. fp32 in-place running mean — cross-frame RAW via `ComputeWrite` import + `ReadStorageImageGeneral`+`WriteStorageImage`. Per-view FNV reset hash (camera VP + snapshot instances + lights + exposure + settings + manual salt). | `9e6aa8e` |
| S2 | **Multi-bounce loop + Russian roulette.** Throughput walk: emission + NEE per vertex, cosine-diffuse bounce, RR after `rrStartDepth` (÷survival), per-bounce indirect firefly clamp. | `ae21337` |
| S3 | **Full Cook-Torrance BRDF + lobe MIS.** D/G/F + `PtBRDFTimesNdotL` matching `pbr.frag`; GGX VNDF specular sampling (Heitz 2018) + cosine diffuse; one-sample lobe MIS (combined-pdf, unbiased). `geom_table` samples the glTF metal-rough map + roughness clamp. | `2e24094` |
| S4 | **Editor panel + convergence UI.** RenderPanel "Path Trace Reference": mode toggle, samples/frame, max bounces, RR depth, firefly clamp, accumulate, Reset button, live "N spp accumulated" readout. | `b200adb` |

---

## Design decisions

- **Top-level `RenderMode`, not a `ShadeMode`.** `ShadeMode` is a post-tonemap debug-blit; a full
  render-path replacement is an A/B toggle like `ShadowingMode`.
- **rayQuery-in-compute megakernel, not SBT raygen.** Mirrors ReSTIR GI; no per-frame SBT rebuild. The
  AS-build→read barrier uses `dstStage = COMPUTE_SHADER_BIT` (NOT `RAY_TRACING` — rayQuery runs in compute).
- **Traces its own primary camera rays.** No raster G-buffer dependency; per-sample sub-pixel jitter gives
  progressive AA, and the raster chain dead-pass-culls cleanly when PT is active.
- **fp32 in-place accumulator (`ptAccum`) + fp16 display copy (`ptColor`).** The running mean keeps fp32
  precision over thousands of samples; `ptColor` (filterable) is what bloom/tonemap sample. The accumulator
  is imported in its left state (`ComputeWrite`) so the RG emits the cross-frame RAW barrier — the proven
  GI-reservoir cross-frame pattern.
- **Robust reset hash.** Camera VP + all snapshot instance transforms/materials + every light + exposure +
  settings + a manual salt — covers every radiance-affecting change, not just camera motion.
- **MIS treatment.** Punctual lights are delta (NEE only), emissive surfaces hit via BSDF only, environment
  via miss only — disjoint, so no cross-technique MIS weights are needed. The genuine MIS is the one-sample
  lobe MIS between the diffuse and specular BSDF-sampling pdfs. The BRDF deliberately matches `pbr.frag`'s
  approximate Smith-G (so the A/B isolates light transport, not BRDF), while the VNDF pdf uses the analytic
  Smith G1 (unbiased — the integrand needn't match the sampler's pdf).
- **TAA off in PT mode.** PT does its own jitter; the projection is held static so the accumulation doesn't
  restart every frame (see bugs).

---

## Bugs caught

- **Accumulation reset every frame** (smoke, `8744f03`). The TAA Halton jitter is baked into the projection
  whenever TAA is enabled; `m_CachedViewProj` (watched by the reset hash) therefore shifted every frame, so
  the accumulator reset every frame and the spp counter was pinned at `samplesPerFrame`. Fix: skip the
  projection jitter in PathTrace mode (the megakernel does its own per-sample jitter).
- **Faceted ("hard normals") shading** (smoke, `aad3b57`). `FetchHitSurface` shaded with the geometric face
  normal. Now barycentric-interpolates the vertex normals (`s.ns`, matching raster `v_Normal`) for the BRDF
  while keeping the geometric normal (`s.ng`) for robust ray-origin offsets. ReSTIR GI keeps its geometric
  secondary normal (`hs.ng`) — behavior unchanged.
- **skyboxIntensity missing from the reset hash** (S1 review). Env-on-miss scales by it; an exposure edit
  would otherwise leave a stale accumulation. Folded into the hash.

---

## Files touched

**Engine — new:** `subsystems/PathTraceSubsystem.{h,cpp}`, `settings/PathTraceSettings.h`; shaders
`path_trace.comp`, `common/geom_table.glsl` (+ `.meta`).
**Engine — modified:** `RenderPipeline.{h,cpp}` (m_PathTrace + BuildGraph seam + needTlas + ViewResources
fields), `ViewResources.cpp` (ptAccum/ptColor + bootstrap clear + pool), `scene/systems/RenderingSystem.h`
(RenderMode + PathTraceSettings + UBO), `subsystems/GlobalSubsystem.cpp` (pathTraceParams + PT-mode jitter
gate), `subsystems/PostProcessSubsystem.cpp` (PT bloom/composite input), `backend/vulkan/TlasBuilder.cpp`
(static_assert message). Shaders `restir_gi_initial.comp` (shared geom-table deref, geometric normal),
`common/globals.glsl` (pathTraceParams).
**Editor:** `panels/RenderPanel.cpp` (Path Trace Reference section).

---

## Verification

Build clean (Debug x64, luth → luthien → Runtime), no new warnings; all `.comp` pass
`glslc --target-env=vulkan1.3`. Two adversarial review workflows (S1 sync, S3 BRDF/MIS). Smoke-tested:
PT toggles through the tonemap chain; converges on a parked camera (spp climbs) + resets on camera/light/
material change + the Reset button; smooth shading matches raster; A/B against ReSTIR DI/GI agrees on
diffuse + adds specular/multi-bounce.

---

## Hand-off / deviations

- **Full PBR (not diffuse-only)** per the locked decision — GGX specular + lobe MIS.
- **Opaque-only RT path.** Cutout (alpha-tested) + transparent objects are treated as opaque by every RT
  consumer (PT + ReSTIR DI/GI + RT shadows all use `gl_RayFlagsOpaqueEXT`); transmission/refraction is a
  Phase-D-class feature. Left as-is — the whole RT path is consistent, so the GI A/B stays apples-to-apples.
- **RAY_TRACED bias-correction visibility re-test** (the restir-gi deferral that "pairs with C.5") is a
  separate follow-up: build the oracle first, then use it to measure the re-test's accuracy gain.
- Git hooks not installed in this workspace — comment policy honoured manually.
