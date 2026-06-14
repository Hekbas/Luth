# slang-material-cleanup (v3.2.3)

**Date:** 2026-06-14 · **Issue:** [#159](https://github.com/Hekbas/Luth/issues/159) (Part of [#157](https://github.com/Hekbas/Luth/issues/157)) · **Series:** `slang-material` Phase-2 follow-ups · **Branch:** `feat/slang-material-cleanup`

## Summary

Finished the three hardening/cleanup items `slang-imaterial` (v3.2.2) deferred: an init-time Slang-reflection **layout drift-guard** for the cross-language struct twins, **full retirement of the Phase-0 spike** (the runtime pixel A/B), and **repointing the bindless-SPIR-V regression gate** from the throwaway spike onto a production shader. Net result: the C++↔Slang `GPUMaterialData` / `GlobalUniforms` layout match is now asserted loudly at boot, the codegen gate covers real shader code, and no spike artifacts (`slang_spike_gi.{comp,slang}`, `slang_spike_diff.comp`, `geom_table.glsl`) remain.

## What shipped

### 1. Layout drift-guard (`MaterialLayoutGuard`)

The C++ `GPUMaterialData` (80 B std430 SSBO) and `GlobalUniforms` (std140 UBO) structs are uploaded raw to the GPU, so their field offsets must stay byte-identical to the `material.slang` / `globals.slang` twins. Before this, the match was guarded only by `static_assert(sizeof(GPUMaterialData)==80)` (material) and nothing at all (globals) — and it holds today only because glm is **not** force-aligned (`LuthMath.h` sets only `GLM_FORCE_RADIANS` / `GLM_FORCE_DEPTH_ZERO_TO_ONE`), so natural C packing happens to equal std140/std430.

- `SlangCompiler::ReflectStructLayout(path, typeName)` — reflects a named struct from a `.slang` module (`module->getLayout` → `findTypeByName` → `getTypeLayout(Default)` → per-field `getOffset(Uniform)` + `getSize`). Reuses the existing fresh-global-session-per-compile `PrepareModule`. Fail-soft: any reflection gap returns `ok=false`.
- `MaterialLayoutGuard::Validate(...)` — compares the reflected offsets/size against a C++ `offsetof` table; a real mismatch logs a per-field `C++ vs Slang` dump + `LH_CORE_ERROR` (asserts in debug), an unavailable reflection logs a WARN and skips (never blocks boot).
- Wired at `MaterialSystem::Init` (`GPUMaterialData`) and `GlobalSubsystem::Init` (`GlobalUniforms`).

### 2. Phase-0 spike retirement (full)

With all 9 production consumers on Slang and the GLSL seam gone, the runtime pixel A/B (GLSL `slang_spike_gi.comp` vs Slang `slang_spike_gi.slang`, diffed on the GPU) had no production reference left to compare against. Retired wholesale: `SlangParityGuard` drops its three compute pipelines, RGBA32F images, diff buffer, descriptor pool/sets, the per-frame `AddPass`, and the `enabled` toggle. `SlangParitySettings` slims to the four gate fields; RenderPanel's section becomes a read-only gate verdict. Deleted: `slang_spike_gi.comp`, `slang_spike_gi.slang`, `slang_spike_diff.comp` (+ `.meta`), and `common/geom_table.glsl` — whose sole remaining `#include` was the spike `.comp`.

### 3. Gate on production code

`SlangParityGuard` is now a deterministic SPIR-V codegen gate that scans **`restir_gi_initial.slang`** (a production rayQuery + bindless + BDA consumer) for the NonUniform decorations + the four capabilities (RuntimeDescriptorArray / PhysicalStorageBuffer / RayQuery / ShaderNonUniform) that slang#10525-class regressions drop. It runs once at init — the SPIR-V is free from the `ShaderLibrary` cache, since `RtRestirGiSubsystem::Init` compiles the shader before the guard inits — and re-scans on that shader's hot-reload (observed **outside** the `||` reload chain, since `RtRestirGi` consumes the reload first and short-circuits it). Offline `slangc` + `spirv-val` confirm the target is VALID and carries all four caps + 11 NonUniform decorations → the gate reports PASS.

## Design decisions / deviations

- **`GlobalUniforms` validated at `GlobalSubsystem::Init`, not `MaterialSystem::Init`** (as issue #159 sketched). The struct lives in `scene/systems/RenderingSystem.h`; checking it from renderer/material would force a renderer→scene layering inversion + a heavy include. `GlobalSubsystem` already owns the UBO upload and includes that header, so it is the coupling-free home. `GPUMaterialData` stays at `MaterialSystem::Init`.
- **Full retirement over "keep slang_spike_gi.slang"** (user-chosen). The deferred-followups note said to keep the Slang spike, but once the gate repoints to production code and the GLSL twin is deleted, the spike `.slang` is only an input to a now-impossible A/B — so it goes too. Nothing dormant left behind.
- **Names kept.** `SlangParityGuard` / `SlangParitySettings` retained (doc comments refreshed) rather than renamed to `SlangCodegenGuard`, to avoid churning `RenderingSystem.h` / RenderPanel / includes for no behavioural gain.
- **Fail-soft guard.** A reflection-API surprise degrades to "guard skipped + WARN", never a boot crash; only a successful-but-mismatching reflection halts (debug) / errors (release). Keeps the diagnostic from becoming a liability.

## Files

- **New:** `renderer/material/MaterialLayoutGuard.{h,cpp}`
- **Engine:** `SlangCompiler.{h,cpp}` (`ReflectStructLayout`), `MaterialSystem.cpp` + `GlobalSubsystem.cpp` (guard call sites), `SlangParityGuard.{h,cpp}` (gate-only rewrite), `SlangParitySettings.h` (slim), `RenderPipeline.{cpp,h}` (drop `AddPass` + out-of-chain reload observe + dead getters + the `needTlas` term), `Material.h` (cross-check comment → `MaterialLayoutGuard`), `Version.h`
- **Editor:** `RenderPanel.cpp` (gate-only readout)
- **Comments:** `TlasBuilder.cpp` + `RtSubsystem.{cpp,h}` + `VolumetricSubsystem.{cpp,h}` + `RtRestirSubsystem.cpp` — orphaned `geom_table.glsl` refs repointed to `material.slang` / `material_bindings_rt.slang`
- **Deleted:** `slang_spike_gi.comp`, `slang_spike_gi.slang`, `slang_spike_diff.comp` (+ `.meta`), `common/geom_table.glsl`
- **Docs:** `ROADMAP.md` (v3.2.3 row + cleared deferred notes), `arch/rendering-pipeline.md` (geom_table.glsl → material.slang)

## Sub-tasks (commit order)

| # | Commit | Notes |
|---|--------|-------|
| 1 | `feat(renderer): reflect Slang struct layout` | `SlangCompiler::ReflectStructLayout` |
| 2 | `feat(renderer): add MaterialLayoutGuard init checks` | guard TU + two `Init` call sites |
| 3 | `refactor(renderer): retire Slang spike A/B` | gate-only `SlangParityGuard` on `restir_gi_initial.slang` |
| 4 | `chore(shaders): delete slang spike + geom_table.glsl` | 4 shaders (+ meta) + orphaned-comment repoint |

## Doc hygiene

Swept `arch/rendering-pipeline.md` for pre-existing `slang-imaterial` (v3.2.2) drift — repointed the converted consumers' stale GLSL filenames to their `.slang` successors (`pbr.frag`→`pbr.slang`, `restir_gi_initial.comp`/`path_trace.comp`→`.slang`, `pbr_transparent_shading.glsl`→`pbr_transparent_shading.slang`). The kept GLSL twins (`froxel.glsl` / `brdf.glsl` / etc.) are still live and untouched; `pbr.vert` stays GLSL; `ARCHITECTURE.md` had no drift; ROADMAP completed-rows keep their period-accurate names.
