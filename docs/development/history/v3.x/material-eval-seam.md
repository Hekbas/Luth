# material-system.M.2 — material-eval-seam

**Date:** 2026-06-10
**Commits:** on `feat/material-eval-seam` — `70eae75` (rename seam), `22dddd7` (migrate pbr.frag), wrap-up
**Umbrella:** [#151](https://github.com/Hekbas/Luth/issues/151)
**Series:** `material-system`, M.2. Mode A — **v3.1.1** PATCH bump, tag-only.

---

## Overview

Finishes the material-system arc's "shared evaluate-at-surface-point BRDF seam." The audit found the seam
**already largely existed**: `common/brdf.glsl` (built during `rt-reflections`, v3.0.17) holds the canonical
Cook-Torrance + GGX-VNDF sampling + pdfs, and `path_trace.comp` + `rt_reflections.comp` already `#include`
and use it. The only remaining hand-duplicated BRDF was **`pbr.frag`**, whose inline
`DistributionGGX`/`GeometrySchlickGGX`/`GeometrySmith`/`FresnelSchlick`/`CalculateLight` were fp32-identical
to the shared copy (same D/G/F/assembly; `PI` 3.14159265359 vs `GI_PI` 3.14159265358979 collapse to the
same float32).

M.2 migrates `pbr.frag` onto the shared seam, so the **raster==RT BRDF parity is now structural** (one
source of truth) instead of a hand-maintained "MUST stay algebraically identical" comment — the exact bug
class that bit emissive in M.1. Output is fp32-identical (no visual change); the win is the deleted
duplication. Also renamed the seam's `Pt*` functions to neutral names since they're no longer
path-tracer-specific (they're called by raster too).

Not duplication, left alone: the `restir_gi`/`rt_reflections` hit-shading is intentionally
Lambertian-only (diffuse-dominant secondary). `brdf_lut.comp` deliberately uses the IBL Schlick-GGX
`k = roughness²/2` (split-sum precompute) — distinct from the direct-lighting `k = (r+1)²/8`, correct as is.

No cornerstone impact (shader-only; no memory/jobs/vulkan/descriptor changes). Planned + reviewed via two
Explore agents (BRDF-duplication inventory + consumer-integration), which is what surfaced that the seam
was already 2/3 done.

---

## Sub-tasks

| # | What landed | Commit |
|---|---|---|
| 1 | **Rename the shared seam to neutral names.** `common/brdf.glsl`: `PtBRDFTimesNdotL`→`EvalBRDFTimesNdotL`, `PtD_GGX`→`D_GGX`, `PtG_SchlickGGX`→`G_SchlickGGX`, `PtG_Smith`→`G_Smith`, `PtF_Schlick`→`F_Schlick`, `PtG1_GGX`→`G1_GGX` (`SampleGGXVNDF`/`PdfDiffuse`/`PdfSpec` already neutral). Reworded the line-1 contract from "must stay identical to pbr.frag" to "single source of truth, called by raster + RT"; added an `#ifndef` include guard. Updated `path_trace.comp` callers (`rt_reflections.comp` calls only `SampleGGXVNDF` → untouched; `restir_gi_initial.comp` doesn't include brdf.glsl → untouched). | `70eae75` |
| 2 | **Migrate `pbr.frag` onto the seam.** Define `GI_PI` + `#include "common/brdf.glsl"`; delete the 5 inline BRDF functions (keep `FresnelSchlickRoughness`, raster-IBL-only); `CalculateLight(...)`→`EvalBRDFTimesNdotL(...)` at the 2 call sites; unify the local `PI`→`GI_PI` (only the DI/GI remod lines remained). pbr.frag −61 net lines. | `22dddd7` |

---

## Design decisions

### The seam is pure BRDF math (no descriptors) — each caller keeps its light loop
A fuller "shade this surface against all lights" helper is infeasible: raster reaches lights via the
clustered Set 3 (`gl_FragCoord` → cluster grid + index SSBOs) with PCF/RT-mask shadows; the RT consumers
use a flat light list (Set 1 remap) + inline `rayQueryEXT` visibility. So the shared seam is exactly the
per-light evaluate `EvalBRDFTimesNdotL(L, radiance, V, N, albedo, metallic, roughness)` — caller supplies
the (shadowed/visible) radiance and owns enumeration. IBL stays caller-side too (raster split-sum LUT;
RT VNDF/cosine sampling). This is why the seam composes across all four paths without `#ifdef` branching.

### fp32-identical, so parity is provable not asserted
`CalculateLight` and `EvalBRDFTimesNdotL` are the same algebra; `0.0001`==`1.0e-4` and `PI`==`GI_PI` in
float32. The migration changes no pixels — but now there is no second copy to drift, so a future BRDF tweak
(energy comp, multiscatter, a different G) lands once and stays raster==RT by construction.

### `GI_PI` constant
brdf.glsl's contract requires the includer to define `GI_PI`. pbr.frag now defines it inline (the RT
consumers get it from `restir_gi_common.glsl`). pbr.frag's only other `PI` uses were the DI/GI remod lines
→ folded onto `GI_PI`; the remaining `PI` mentions are conceptual π in comments.

---

## Files touched

`luth/assets/shaders/common/brdf.glsl` (rename + contract reword + include guard),
`luth/assets/shaders/pbr.frag` (include + delete 5 fns + call rename + PI→GI_PI),
`luth/assets/shaders/path_trace.comp` (caller rename). Docs: `core/Version.h` (3.1.1), `ROADMAP.md`,
this file.

---

## Verification

`glslc --target-env=vulkan1.3` passes on `pbr.frag`, `path_trace.comp`, `rt_reflections.comp`,
`restir_gi_initial.comp` (the brdf.glsl + geom_table consumers) after both commits; grep confirms zero
`Pt*` names and zero deleted-function references remain. Shader-only change (engine compiles shaders at
runtime, not via MSBuild) + a trivial `Version.h` constexpr bump → glslc is the gate, no C++ rebuild
needed. **Smoke (visible-no-op sanity):** lit raster surfaces identical to v3.1.0; `RenderMode::PathTrace`
still matches raster; point/dir lighting + IBL unchanged.

---

## Hand-off / deferred

- Broader "evaluate-at-surface-point" unification — a shared SURFACE-SAMPLING path between `pbr.frag`'s
  inline material reads and `geom_table.glsl`'s `FetchHitSurface` — is a separate, harder effort (the Set
  1/2 vs Set 3/4 binding divergence mirrors the light-loop divergence). Not attempted here.
- Arc continues: M.3 `cutout-rt`, M.4 `transparency-tier`. Deferred: emissive-as-area-lights; Slang `IMaterial` spike.
- Local-only per the active workflow: v3.1.0 + v3.1.1 sit merged+tagged on local `main`, unpushed; the
  milestone Release decision is still open.
- Git hooks not installed in this workspace — comment/commit policy honoured manually.
