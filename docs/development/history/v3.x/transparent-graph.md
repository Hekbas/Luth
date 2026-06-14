# transparent-graph (v3.2.7)

**Date:** 2026-06-14 · **Issue:** Part of [#157](https://github.com/Hekbas/Luth/issues/157) · **Series:** `slang-material` / Materials arc M1 · **Branch:** `feat/transparent-graph`

## Summary

A node-graph material set to RenderMode `Transparent`/`Fade` rendered its graph correctly in
every RT path (path-trace, reflections, GI) but rendered **stock** — ignoring the graph — in the
raster transparent pass. That broke the `slang-material` series' load-bearing invariant, **raster
== RT**: the same material looked different depending on whether it was rasterised or ray-traced.
`material-node-editor` (v3.2.5) shipped with this flagged as a known deferral.

This effort closes the gap by routing the transparent raster decode through the **same
structure-keyed variant registry the RT hit already uses**. The fix is two files / ~5 effective
lines: the shared transparent shading seam decodes via `EvalGraphVariant` instead of the stock
`EvalMaterialChannels`, and codegen adds the two transparent shaders to the set it recompiles when
a new graph structure regenerates the registry. No per-material transparent pipelines, no
`DrawCommand`/pass-loop/`Material`-struct/C++ changes, no RT changes.

## What shipped

### 1. Transparent raster decode through the variant registry

`common/pbr_transparent_shading.slang::EvalTransparentSurfaceColor` is the single shading body both
transparent entries route through — `pbr_transparent.slang` (sorted back-to-front blend) and
`pbr_oit_store.slang` (PPLL store). It hardcoded the stock decode:

```slang
RasterFetch rf;
MaterialInputs mi = EvalMaterialChannels<RasterFetch>(m, uv0, uv1, rf);
```

Now it mirrors `material_bindings_rt.slang`'s `FetchHitSurface` exactly:

```slang
RasterFetch rf;
rf.paramBase = materialIndex * MAT_GRAPH_STRIDE;
MaterialInputs mi = EvalGraphVariant<RasterFetch>(GraphVariant(m.flags), m, uv0, uv1, rf);
```

Plus `import mat_graph_registry;`. Because both transparent entries share this one seam, the single
decode swap fixes both the Sorted and OIT modes. Variant 0 (every non-graph material) resolves to
the registry's default arm → stock `EvalMaterialChannels`, so non-graph transparent materials are
byte-identical. The generated bodies read their constants through `fetch.Param(slot)` =
`gMatParams[paramBase + slot]`; `gMatParams` is already bound as Set 2 binding 1 in both transparent
draw loops (it is `MaterialSystem`'s set, bound at every raster pipeline), so the data seam was
already present — only the decode call had to change.

### 2. Transparent shaders join the registry-reload set

When a brand-new graph **structure** appears, `MaterialGraphCodegen::GenerateAndCompile`
regenerates the project `mat_graph_registry.slang` and reloads every shared shader that decodes
through it. `ScheduleRtReload` (now renamed `ScheduleGraphConsumerReload`, and `kRtConsumers` →
`kGraphConsumers`, since the set is no longer RT-only) gains `"pbr_transparent.slang"` and
`"pbr_oit_store.slang"`. `ShaderLibrary::Reload` recompiles them from disk against the regenerated
registry, then the existing reload fan-out runs `TransparencySubsystem::OnShaderReloaded` (already
handling both names → `DeferredInvalidateShader` on its four PipelineManagers). Zero new reload
wiring. A value edit or clone is a structure-hash **hit** → no registry regen, no reload (the
constants are data in `gMatParams`), so the only recompile is the one-time per-new-structure event
already paid by the RT megakernels.

## Design decisions / deviations

- **Variant registry, not per-material transparent consumers (deviation from the v3.2.5 deferred
  note).** The `material-node-editor` history sketched the follow-up as "generating
  transparent-flavoured consumer variants" — mirroring the opaque raster path, which binds a
  per-material generated pipeline per graph and is **uncapped**. That sketch predated
  `graph-param-buffer` (v3.2.6), which introduced the structure-keyed variant registry. Per-material
  transparent pipelines would have made transparent-raster **diverge from RT past the 16-structure
  cap** (transparent-raster honours the 17th structure; RT, capped, falls to stock) — a brand-new
  raster≠RT break, the exact thing the effort exists to remove. Routing through the registry shares
  RT's decode path *and* its cap, so transparent-raster == RT by construction (for the 17th+
  structure both fall to stock — still equal). It is also a far smaller surface. This is the kind of
  ROADMAP/deferred-note re-validation the plan discipline mandates: the sketch is from scoping time;
  the newer primitive wins.
- **Decode-at-the-seam, not at the entry.** The opaque path slimmed `pbr.slang` to decode then
  delegate to `PbrShadeSurface(mi, …)`. The transparent seam already does its decode *internally*
  (`EvalTransparentSurfaceColor` calls the decode then shades), so the swap stays inside the seam and
  the two entry shaders are untouched. Minimal churn, and both entries inherit the fix.
- **Asymmetry with opaque raster is intentional.** Opaque-raster decodes via per-material pipelines
  (uncapped, hot path — avoids a per-fragment switch on the bulk of geometry); transparent-raster
  and RT decode via the registry switch (capped at 16, lighter pass). Transparent chose the
  RT-style mechanism precisely because it is parity-exact with RT and far smaller — the right
  trade for a low-draw, fill-bound pass.

## Bugs fixed along the way

None — the change is additive and the integration points were verified before implementing
(reload fan-out includes transparency outside the `||` chain; `EnsureMaterialRegistered` runs
render-mode-agnostically so transparent materials already get a variant at load; `gMatParams` is
already bound in both transparent loops; `mat_graph_registry` lives in `registry/` not `common/`,
so a `common/`-resident importer resolves to the project override / engine default correctly).

## Files

- **Engine shaders:** `assets/shaders/common/pbr_transparent_shading.slang` (`import
  mat_graph_registry`; decode → `EvalGraphVariant<RasterFetch>` + `paramBase`)
- **Engine:** `renderer/material/MaterialGraphCodegen.cpp` (`ScheduleRtReload` →
  `ScheduleGraphConsumerReload`, `kRtConsumers` → `kGraphConsumers`, + the two transparent shader
  names), `core/Version.h`
- **Docs:** `ROADMAP.md`, this history file

## Sub-tasks (commit order)

| # | Commit | Notes |
|---|--------|-------|
| 1 | `feat(material): honor node graph in raster transparent pass` | Decode-seam swap to `EvalGraphVariant` + the two transparent shaders join the registry-reload set (one atomic <10-LOC fix across both files) |

## Verification

- Built Debug x64 clean via direct MSBuild — only the pre-existing warnings (C4267 size-narrowing in
  `Model.cpp`, C4996 `getenv`/`strncpy`, C4244 chrono in `Editor.cpp`, the `vulkan-1.lib` LNK4006).
  `MaterialGraphCodegen.cpp` recompiled clean.
- **Offline `slangc`** against the engine session flags (`-profile spirv_1_5 -fp-mode precise
  -matrix-layout-column-major -emit-spirv-directly`, search paths src → common → registry): both
  `pbr_transparent.slang` and `pbr_oit_store.slang` emit SPIR-V (a) against the engine **default
  registry** (variant 0 / stock — exercises the previously-uninstantiated `EvalGraphVariant<RasterFetch>`
  specialization) and (b) against a hand-authored **project registry override** whose variant-1 arm
  dispatches a real `EvalGraph_<hash><RasterFetch>` (using `fetch.Param` + `fetch.Sample`) — machine-checking
  the raster==RT decode path at the transparent tier. The lone warning is the benign 41012 capability
  auto-upgrade the engine already suppresses.
- **Runtime smoke (user):** a transparent graph material shows its channel routing in the viewport
  in **both** Sorted and OIT transparency modes, identically to Path-Trace (raster == RT); a
  `Const`/`Remap` value drag updates live with no recompile log; a brand-new structure on a
  transparent material picks up after the reload (no restart) and a clone shares its variant;
  non-graph transparent materials are visually unchanged; `SlangParityGuard` stays green.
