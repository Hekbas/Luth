# slang-imaterial (v3.2.2)

**Date:** 2026-06-14 · **Issue:** [#158](https://github.com/Hekbas/Luth/issues/158) (Part of [#157](https://github.com/Hekbas/Luth/issues/157)) · **Series:** `slang-material` Phase 2 (the keystone) · **Branch:** `feat/slang-imaterial`

## Summary

Collapsed the hand-mirrored material-decode triplet (C++ `GPUMaterialData` + GLSL `pbr_surface.glsl` + `geom_table.glsl` `GtMaterial`) into **one bounded `material.slang` module**, evaluated in **two tiers from a single generic** (`EvalMaterialChannels<F : ITexFetch>` — `RasterFetch` auto-mips, `RayFetch` samples LOD 0), and converted **all 9 seam consumers** (raster + RT + transparent + volumetric) to import it. The production GLSL seam is deleted; the rest of the corpus stays GLSL on shared twins.

Validated per-shader offline (`slangc` + `spirv-val` mirroring the engine session flags) and end-to-end in-engine across two user smoke rounds (opaque PBR, CSM/RT shadows, IBL, ReSTIR DI/GI, RT reflections, path-trace, transparent blend + OIT, volumetric fog).

## Architecture

- **`common/material.slang`** — single source for the 80 B `GPUMaterialData`, the bounded `MaterialInputs` surface (baseColor / normal / metallic / roughness / occlusion / emissive — derivative/screen-space ops excluded by construction), `interface ITexFetch`, the generic `EvalMaterialChannels<F>` + `ApplyNormalMap<F>` decode, the RT geometry-gather (`GatherHitGeometry`, ported from the spike), the BDA `GtGeomEntry`, `HitSurface`, and `GT_VIS_*` masks.
- **Two binding headers** carry the set asymmetry (slang#8063 forbids auto-binding the variable-count bindless heap, so sets are hand-pinned): `material_bindings_raster.slang` (bindless Set 1, material Set 2) and `material_bindings_rt.slang` (bindless Set 4, material Set 3) — each declares the fetch-policy impl + the set-specific `FetchHitSurface` / alpha-test wrappers. This is a direct translation of the old `pbr_transparent_shading.glsl` `#define`-alias trick.
- **Two-tier link-time specialization:** the raster/RT tier is one of the "1-4 axes" — `EvalMaterialChannels<RasterFetch>` vs `<RayFetch>` specialize at link time, no dynamic branch, no duplicated body.

## Sub-tasks (commit order)

| # | Commit | Notes |
|---|---|---|
| Toolchain | `let .slang import modules from common/` | added `shaders/common` to the Slang session search path |
| Module | `add material.slang bounded eval module` | struct + MaterialInputs + ITexFetch + EvalMaterial + geometry-gather + 2 binding headers |
| Commons | `port globals + brdf`, `port ReSTIR commons`, `port pbr_lighting`, `port froxel + oit_common` | Slang twins of the shared GLSL includes |
| Loader fix | `thread-safe .slang asset compilation` | see "The thread-safety saga" below — squashed from 3 |
| Consumers | `rt_sun_shadows`, `restir_gi_initial`, `rt_reflections + path_trace`, `pbr (keystone)`, `volumetric_inject_scatter`, `transparent pair`, `restir_initial` | 9 entry shaders → `.slang`; each subsystem's load + hot-reload repointed |
| Deletion | `delete production GLSL material seam` | removed `pbr_surface`/`pbr_lighting`/`pbr_transparent_shading.glsl` + the 9 orphaned GLSL originals |

## The thread-safety saga (the bug worth remembering)

The first in-engine boot crashed non-deterministically: sometimes `key already exists in Dictionary` for `globals.slang`, sometimes a raw access-violation, sometimes a self-contained shader reporting its **own** types as undefined. Three fixes were attempted (module-classification, `loadModule` instead of `loadModuleFromSourceString`, both independently reasonable) before an **offline C++ repro** isolated the true cause: the process-global `slang::IGlobalSession` is **not safe for concurrent module loading**, and the asset pipeline compiles `.slang` on multiple threads. Phase 1 had assumed serialized compiles; the asset pipeline broke that assumption. The repro proved a shared session crashes under 4 concurrent threads while a **fresh global session per compile** survives — so the fix (in `SlangCompiler::PrepareModule`) is per-compile isolation, lock-free (a `std::mutex` would block worker threads, violating the fiber-system rule). The three exploratory fix commits were squashed into one. Lesson: any in-process Slang integration that compiles concurrently needs per-compile global-session isolation.

## Design decisions

- **Generics over a fetch policy**, not preprocessor or hand-mirrored wrappers — "type-check once, specialize at link time" (the locked decision). Raster=fragment / RT=compute are different stages, so `.Sample` (implicit-derivative) vs `.SampleLevel(.,0)` can't share one compiled body without compile-time specialization.
- **TBN across the GLSL→Slang vertex/fragment boundary:** `pbr.vert` stays GLSL and emits `mat3 v_TBN` at locations 3-5; the Slang fragments read it as **three separate `float3` (T/B/N)** varyings — sidesteps the HLSL row-major matrix-constructor convention the spike calls out.
- **LightSSBO via `ByteAddressBuffer` + explicit offset loads** (spike-style) rather than struct-mapped buffers — the header+flexible-array layout isn't a clean `StructuredBuffer<T>`, and byte loads are std430-drift-free.
- **C++ `GPUMaterialData` retained** (the SSBO upload format) + the cross-language SSBO twins (`globals`/reservoir/etc.) kept byte-matched by construction; validated by `static_assert(sizeof==80)`, offline reflection-clean compiles, and the in-engine smoke.

## Files

- **New (`luth/assets/shaders/common/`):** `material.slang`, `material_bindings_raster.slang`, `material_bindings_rt.slang`, `globals.slang`, `brdf.slang`, `restir_common.slang`, `restir_gi_common.slang`, `restir_di_target.slang`, `pbr_lighting.slang`, `froxel.slang`, `oit_common.slang`, `pbr_transparent_shading.slang`
- **New consumers (`luth/assets/shaders/`):** `pbr.slang`, `rt_sun_shadows.slang`, `restir_gi_initial.slang`, `rt_reflections.slang`, `path_trace.slang`, `volumetric_inject_scatter.slang`, `pbr_transparent.slang`, `pbr_oit_store.slang`, `restir_initial.slang`
- **C++:** `SlangCompiler.cpp` (search path + file-based `loadModule` + per-compile global session), `FileSystem.cpp` (`common/*.slang` = import-only modules), 8 subsystem loaders repointed (`Geometry`/`RtRestirGi`/`Reflections`/`PathTrace`/`Rt`/`Volumetric`/`Transparency`/`RtRestir`), `Version.h`
- **Deleted:** `pbr_surface.glsl`, `pbr_lighting.glsl`, `pbr_transparent_shading.glsl` + the 9 orphaned GLSL consumer originals (+ `.meta`)

## Deferred follow-ups (tight next session)

These are hardening + a Phase-0-artifact cleanup, intentionally NOT rushed into init-time C++ at the end of a long effort (the render smoke can't validate them):

- **Slim layout drift-guard** (plan task 4) — reflect `material.slang`'s `GPUMaterialData` + the `globals` twin at `MaterialSystem` init and assert against the C++ offsets. Today the layouts are covered by `static_assert` + offline reflection-clean compiles + the smoke; this adds a loud init-time check against future drift.
- **SlangParityGuard on a real material shader** (plan task 7) — the guard still scans the Phase-0 spike (`slang_spike_gi.slang`); repoint it at a production material shader.
- **Retire the Phase-0 spike** — `slang_spike_gi.comp` (the GLSL A/B twin) is the last consumer of `geom_table.glsl`; retiring the guard's GLSL A/B frees the final seam file. The production material path no longer touches `geom_table.glsl` — it lingers only for this diagnostic.
