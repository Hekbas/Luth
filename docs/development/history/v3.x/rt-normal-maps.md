# material-system.M.5 — rt-normal-maps

**Date:** 2026-06-11
**Commit:** on `fix/rt-normal-maps` — single shader-seam commit
**Umbrella:** [#151](https://github.com/Hekbas/Luth/issues/151) · **Closes:** [#153](https://github.com/Hekbas/Luth/issues/153)
**Series:** `material-system`, M.5. Mode A — **v3.1.4** PATCH bump, tag-only.

---

## Overview

RT hit shading evaluated the surface flat. `FetchHitSurface` (`common/geom_table.glsl`) built its shading
normal `ns` from barycentric vertex normals only and never sampled the material normal map, so the path
tracer and RT reflections shaded normal-mapped surfaces faceted while raster (`pbr.frag` via
`pbr_surface.glsl`) perturbs through a TBN. The same parity sweep that found #153 surfaced a sibling gap in
the identical seam: the baked occlusion map is never read at RT hits either, though raster applies it to the
IBL ambient term. Both were fixed together — this is the RT half of the evaluate-at-surface-point seam (the
material-system arc's thesis) catching up to the raster half.

## Parity matrix (the audit finding behind this)

| Material input | Raster | RT hit (before) | RT hit (after) |
|---|---|---|---|
| Normal map | `pbr_surface.glsl` TBN perturb | vertex normal only | TBN perturb — matches |
| Normal transform | inverse-transpose (`pbr.vert:69`) | plain `mat3(o2w)` | inverse-transpose — matches |
| Occlusion map | sampled → IBL ambient | never sampled | `HitSurface.ao` → rt_reflections IBL ambient |

## Mechanism / decisions

- **TBN at the hit.** Object-space vertex tangent at floats 10-12 (present in both the 52 B `Vertex` and
  84 B `SkinnedVertex` layouts — first 13 floats identical), barycentric-interpolated. World tangent via
  plain `mat3(o2w)`, Gram-Schmidt against the shading normal, `B = cross(ns, T)`. Mirrors `pbr.vert`'s TBN
  exactly; there is no tangent-`w` handedness in the vertex layout, so neither path uses one.
- **Inverse-transpose normal matrix.** `mat3(transpose(inverse(mat3(o2w))))` replaces the prior plain
  `mat3(o2w)` for the vertex normal. `pbr.vert:69` always did this; the RT side was the one approximating
  ("negligible skew"). Closes a silent raster≠RT divergence under non-uniform scale — independent of, but
  folded with, the normal-map fix since both touch the same line.
- **Two-sided facing moved after the perturb.** `ns` is faced to the geometric side (`dot(ns, ng) < 0`)
  *after* the normal-map sample — the RT analog of `pbr.frag`'s `gl_FrontFacing` flip, which also runs after
  normal mapping.
- **Occlusion fold scoped by what raster actually does.** `pbr.frag` multiplies `ao` into the IBL ambient
  (167) and the RT-reflection composite (180) **only** — never the direct, ReSTIR DI (152), or ReSTIR GI
  (163) terms. So `HitSurface.ao` is applied at exactly one RT site: `rt_reflections.comp::ShadeHit`'s
  diffuse-IBL ambient. The path tracer omits it — a multi-bounce reference resolves occlusion
  geometrically, so baked AO would double-darken (the same reason PT already omits screen-space GTAO).
- **ReSTIR GI keeps the geometric secondary normal.** `restir_gi_initial.comp:110` deliberately shades the
  secondary with `hs.ng`, not `hs.ns`, and raster doesn't `ao` the GI term — so neither the normal-map nor
  the occlusion change touches the GI path (a denoised diffuse bounce where normal-map micro-detail washes
  out). Left as-is, by design — the "decide whether GI adopts ns" task resolved to "no".
- **B1 dead-index housekeeping (Phase-0 fold).** `alphaIndex` / `specularIndex` / `thicknessIndex` are
  written by the importer but sampled by no shader (no opacity-map / spec-gloss / SSS path). Marked
  `reserved — unsampled` in `Material.h`, `pbr_surface.glsl`, and `geom_table.glsl` rather than removed —
  std430 padding keeps `GPUMaterialData` at 80 B either way, and thickness/alpha have plausible future homes.
- **NaN guard.** The perturb is gated on a non-degenerate tangent so a normal-flagged-but-tangentless mesh
  can't poison the path tracer's fp32 accumulation (one NaN persists across all future frames).

## Scope

Functionally **shader-only**: `occlusionIndex` + the `HAS_*` / UV-set flags already exist on
`GPUMaterialData` and are importer-written; raster already consumes them. No descriptor / pass / buffer
changes — the RenderGraph + tagged-heap cornerstones are untouched. The lone C++ touch is a doc comment in
`Material.h`.

## Verification

- `glslc --target-env=vulkan1.3` clean on every `geom_table.glsl` includer: `path_trace.comp`,
  `restir_gi_initial.comp`, `rt_reflections.comp`, and the `GT_NO_RESOURCE_DECLS` aliasers
  `pbr_transparent.frag` + `pbr_oit_store.frag` (the new `HitSurface.ao` field + constants don't break the
  alias path), plus raster `pbr.frag`.
- Runtime smoke (pending user gate): raster vs `RenderMode::PathTrace` vs RT reflections on a normal-mapped +
  occlusion-mapped material — normal detail should appear in PT + reflected hits matching raster; AO should
  darken the surface in reflections (PT stays brighter by design).

## Files

**Modified:** `shaders/common/geom_table.glsl` (constants, `HitSurface.ao`, TBN + normal-map + occlusion +
inverse-transpose), `shaders/rt_reflections.comp` (ao on the IBL ambient), `shaders/common/pbr_surface.glsl`
(B1 comment), `renderer/material/Material.h` (B1 comment), `core/Version.h` → 3.1.4.
