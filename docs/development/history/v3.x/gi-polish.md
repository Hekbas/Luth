# rt-renderer.C.3 follow-ups — gi-polish

**Date:** 2026-06-09
**Commits:** 4 direct to `main` (TLAS `42c5bdd`, sun GI `4cdf498`, reservoir viz `03e650e`, zero-weight fix `80602bf`)
**Umbrella:** [#127](https://github.com/Hekbas/Luth/issues/127)
**Series:** `rt-renderer`, post-C.3 polish. **v3.0.15** tag-only, no Release. Landed direct-to-`main` (single-commit each; no branch per the solo-dev GitOps).

---

## Overview

Four small follow-ups after C.3 `restir-gi` (v3.0.14) — three planned during the C.3 wrap-up, one a real GI bug the new debug viz immediately surfaced.

| # | Commit | What | Kind |
|---|---|---|---|
| 1 | `42c5bdd` | **TLAS for any RT consumer.** The TLAS build was gated on `runRtShadows`, so ReSTIR DI/GI silently no-op'd under CSM shadow mode (ran against the empty TLAS). Now `needTlas = runRtShadows \|\| GetRestir().IsEnabled() \|\| GetRestirGi().IsEnabled()`; the RT sun-shadow trace stays RT-only. Pre-existing since C.1 (affected shipped DI too). | fix |
| 2 | `4cdf498` | **Sun light in the GI bounce.** `restir_gi_initial.comp` adds a deterministic directional NEE at the secondary hit (toward-sun shadow ray + `albedo/π·color·intensity·NdotL`). GI was point-lights + emissive only; the sun (usually the dominant light) now contributes its indirect bounce. | feat |
| 3 | `03e650e` | **GI reservoir M·age debug viz.** `ShadeMode::RestirGiReservoir` → a fullscreen graphics pass (mirrors ClusterVizPass) heat-mapping the spatial reservoir's `M` (confidence) + `age` (staleness, dims) over LDR. Reads the CONCURRENT spatial reservoir SSBO on the graphics queue (frame-semaphore visible, runs post-GeometryPass). New `restir_gi_reservoir_viz.frag`; ScenePanel debug radio. | feat |
| 4 | `80602bf` | **Hold the GI sample point on zero-weight reservoirs** (found via #3). | fix |

---

## The #4 bug (and how #3 caught it)

With #3 live, the reservoir viz showed a hard **world-space confidence split at the `z = 0` plane** — the `worldPos.z < 0` half accumulated `M`, the `+Z` half stayed blue, persistent on a static camera, present **even with no lights**. Root cause:

A secondary hit with `L_o = 0` (unlit / shadowed → `w = 0`) never passed the WRS conditional store (`wSum > 0 && rnd·wSum ≤ w`), so the reservoir kept its **reset** `samplePos = (0,0,0)` and `sampleNormalOct = (0,0)`. `OctDecode((0,0))` is exactly `(0,0,-1)`, so the reconnection Jacobian's `cosN = max(0, -worldPos.z/|worldPos|)` — positive (reuse accepted, `M` grows) only where `worldPos.z < 0`, clamped to 0 (Jacobian → 0 → rejected) where `worldPos.z > 0`. A clean world-plane reuse bias, radiance-independent.

**Fix:** enforce the invariant **`M > 0 ⇒ samplePos valid`** at every stage — `GIReservoirUpdate`/`GIReservoirMerge` seed the sample on the first candidate/merge regardless of weight, and the spatial canonical (domain 0) always seeds. An unlit hit is a *confident zero* (valid `x_s`, `W = 0`), not an empty reservoir. The first attempt fixed only the initial pass; the temporal/spatial combines re-introduced the origin, so the fix had to cover the shared helpers + the spatial inline. Lit path is byte-identical (a `w > 0` first sample was always stored; BASIC `piSum` yields `W = 0` for unlit either way). Latent in shadowed regions of lit scenes too — removing the lights just made it global.

---

## Files touched

- **#1** `RenderPipeline.cpp` (TLAS gate).
- **#2** `restir_gi_initial.comp` (sun NEE).
- **#3** `RtRestirGiSubsystem.{h,cpp}`, `RenderPipeline.{h,cpp}`, `ViewResources.cpp`, `RenderingSystem.h`, `ScenePanel.cpp`, new `restir_gi_reservoir_viz.frag` (+ `.meta`).
- **#4** `common/restir_gi_common.glsl` (`GIReservoirUpdate`/`GIReservoirMerge`), `restir_gi_{initial,spatial,temporal}.comp`.

## Verification

Build clean (Debug x64); all GI `.comp` + the viz `.frag` pass `glslc`. Smoke-tested per item under `LUTH_VALIDATION`: #1 DI/GI work under CSM; #2 sun bounce visible + shadowed; #3 heatmap renders, no sync-val on the cross-queue reservoir read; #4 the `z = 0` split is gone (uniform `M` with no lights) and lit GI unchanged.

## Note / deferred

The new debug-viz dimming reads `age`; `M` (the heat ramp) saturates at `temporalMCap·(spatialNeighbours+1)+1`. Fog point-light shadows remain **Phase D.2 `volumetric-rt-shadows`** (RT shadow rays from fog voxels — the proper fix for the currently-unshadowed point lights in the fog; the sun already has CSM there).
