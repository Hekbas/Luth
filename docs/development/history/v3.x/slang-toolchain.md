# slang-material.Phase-1 — slang-toolchain

**Date:** 2026-06-13
**Version:** v3.2.1 (Mode A intermediate — tag-only, no Release)
**Branch:** `feat/slang-toolchain` — `5cc50f8` (compile seam), `1910560` (asset type), `fed350a` (rename), `d107e60` (guard dogfoods asset path), `8381c4d` (byte-identical ray math + verdict)
**Issue:** [#157](https://github.com/Hekbas/Luth/issues/157) · `slang-material` series, Phase 1 (spec: `docs/development/epics/slang-material.md`, local)

---

## Overview

Phase 0 (the spike, #156) stood up an in-process `SlangCompiler` *alongside* libshaderc and proved Slang on the RT hot path, but `.slang` was not yet a first-class asset: the importer infers stage from the file extension, and a `.slang` carries its stage in a `[shader("...")]` attribute, so `ShaderImporter` bailed on it; `ShaderWatcher` ignored `.slang`; and the A/B harness compiled its Slang shader by a direct call that never hot-reloaded. Phase 1 closes the toolchain so GLSL and Slang are peers through the existing asset pipeline, and promotes the throwaway spike into a durable, quiet **bindless-SPIR-V parity regression guard**.

Everything reuses existing primitives — the UUID/`.meta`/artifact pipeline + `ImportDirty`, `AssetSerializer`'s v2 single-stage `ShaderHeader`, `ShaderLibrary`, the `VulkanShader::Reload` → `RenderPipeline` reload-callback chain, and `SlangCompiler`. No new allocator, ring buffer, sync primitive, or descriptor pattern; no architectural departure.

---

## Sub-tasks

| # | What landed | Commit |
|---|---|---|
| 1 | **Stage-aware Slang compile + importer facade.** `SlangCompiler::CompileReflectStage` returns `{spirv, stage}`, reading the stage off the linked program's reflection (`getLayout()->getEntryPointByIndex(0)->getStage()` — free after link); entry-count ≠ 1 / no-`main` skips quietly. `ShaderCompiler::CompileStaged` facade routes `.slang`→reflection, GLSL→extension, so `ShaderImporter` stays language-agnostic. Extracted a shared `PrepareModule` helper across the three `SlangCompiler` entry points. | `5cc50f8` |
| 2 | **`.slang` as a Shader asset + watcher.** `FileSystem::ClassifyFileType` maps `.slang`→`AssetType::Shader` (UUID + `.meta` auto-created by the scan; Shader meta defaults apply). `ShaderWatcher` filter accepts `.slang`. | `1910560` |
| 3 | **Rename `SlangSpike*` → `SlangParityGuard`.** Pure mechanical rename across 7 sites + 3 file renames: class `SlangParityGuard`, struct `SlangParitySettings`, `m_SlangParity`/`GetSlangParity`, RG pass + ImGui ids, editor section "Slang Parity Guard". Solution regenerated. | `fed350a` |
| 4 | **Guard rides the `.slang` asset path + retire link probe.** `EnsureInitialized` loads `slang_spike_gi.slang` via `ShaderLibrary::LoadEngine` (UUID + artifact + hot-reload), and `OnShaderReloaded` rebuilds `m_SlangPipeline` on a `.slang` change. Deleted the multi-entry link-spec probe (`RunLinkSpecCheck` + `slang_spike_link.slang`); kept `CompileModuleEntries` for Phase 2. | `d107e60` |
| 5 | **Deterministic SPIR-V regression gate; pixel A/B demoted to a diagnostic.** `CheckSlangSpirv` walks the compiled `slang_spike_gi.slang` SPIR-V for the slang#10525 signals — required caps (RuntimeDescriptorArray / PhysicalStorageBuffer / RayQuery / ShaderNonUniform) + a nonzero `NonUniform` decoration count (12 at `-O none`, optimization-dependent) — setting the verdict at init + on `.slang` reload. The runtime pixel A/B stays default-OFF as an informational visual diff only (view-/TAA-jitter-dependent). The byte-identical-ray-math + ULP/abs-tolerance attempt was reverted after the smoke showed the pixel diff can't be a deterministic gate. | `8381c4d` → smoke rework |

---

## Design decisions

### The stage lives in the source, not the extension — resolve it by reflection
GLSL's stage falls out of `.vert`/`.frag`/`.comp`; a `.slang` entry's stage is in its `[shader("...")]` attribute. Rather than string-scan the source or compile twice, `CompileReflectStage` reads the stage off the **already-linked** program (`getLayout()` is free once the module is linked for codegen). The importer never learns Slang exists — it calls the `ShaderCompiler::CompileStaged` facade, which branches on extension. This keeps the asset layer language-agnostic and the language dispatch in one place (`ShaderCompiler`), matching the spec's "`.slang` dispatch in `ShaderCompiler`".

### The guard dogfoods the new path — and that's how `.slang` hot-reload is validated
The cleanest way to exercise the full `.slang` asset pipeline end-to-end is to make the guard's own Slang shader the first real consumer: `slang_spike_gi.slang` now loads through `ShaderLibrary::LoadEngine` (so it gets a UUID + artifact and rides `ShaderWatcher`), and `OnShaderReloaded` gained a `.slang` branch that rebuilds `m_SlangPipeline` against the shared 5-set layout. Editing the `.slang` and saving now hot-swaps the Slang side of the A/B exactly like the `.comp` reference — the same chain, no special-casing.

### Multi-entry `.slang` is not a single-stage asset — retire the link probe
The Phase-0 link-spec probe `slang_spike_link.slang` has entries `csMain`+`fsMain` and no `main`; once `.slang` is a scanned asset, `ImportDirty` would try to import it as a single stage and log an error every cold boot. Its slang#9578 gate already passed, and Phase 2 exercises `CompileModuleEntries` against a real `IMaterial` module, so the probe is retired (the function + shader deleted; `CompileModuleEntries` stays in `SlangCompiler`). `CompileReflectStage` also skips any future multi-entry/non-`main` `.slang` quietly as defence-in-depth.

### The gate is a SPIR-V scan, not a pixel diff (what the smoke taught)
The spike promoted a runtime pixel A/B (dispatch the GLSL and Slang shaders, diff the images) as the parity signal, and Phase 1 first tried to make it a clean pass/fail — byte-identical camera-ray math (hand-expanded `mat×vec`, GLSL `precise`), then a ULP, then an absolute-delta tolerance. The runtime smoke killed that on two counts. **ULP distance is meaningless on HDR-linear output** — a sub-1e-4 difference straddling zero reads as ~1e9 ULP (observed `maxUlp ≈ 9.6e8` at `maxAbsDiff ≈ 0.05`). And more fundamentally the pixel diff is **non-deterministic**: TAA jitters the camera sub-pixel each frame, and the two compilers still lower the `mat×vec` ~1 ULP apart, so the jitter sweeps rays across silhouettes where that 1 ULP flips the committed hit — the diff flickers frame-to-frame and varies by view. No tolerance cleanly separates that floor from a real regression.

So the gate moved to where the hazard actually lives. slang#10525 is a **SPIR-V codegen** bug — the `NonUniform` decoration vanishing/misplacing on the bindless texture accesses. `CheckSlangSpirv` walks the compiled `slang_spike_gi.slang` SPIR-V (a few KB, microseconds, no GPU) and asserts the required capabilities are present (`RuntimeDescriptorArray` / `PhysicalStorageBufferAddresses` / `RayQueryKHR` / `ShaderNonUniform`) and the `NonUniform` decoration count is nonzero (12 under the engine's `-O none` session — the exact number is optimization-dependent, 14 under slangc's default opt, which is why the gate is *presence*, not a pinned count); a regression zeroes or strips them. Deterministic, view/TAA/frame-independent, run when the guard initialises and on every `.slang` hot-reload — one INFO on pass, one WARN on regression. The byte-identical-ray-math shader edit was **reverted**: it never achieved bit-identical rays (the persistent ~1-ULP difference still flips hits under jitter) and only spread the noise across texture edges. The runtime A/B harness stays as a default-OFF **visual diagnostic** — handy for eyeballing parity when converting shaders in Phase 2 — but carries no verdict, so it costs nothing per frame when idle.

### Accepted consequence: `.slang` now compiles at boot when dirty
Making `.slang` a scanned asset means `AssetManager::ImportDirty()` (App boot, and project load) compiles every dirty `.slang` — i.e. on a cold Library or after editing the shader — which loads `slang-compiler.dll` at boot. Warm boots hit the cached artifact and never load it. This retires the spike's "no DLL until toggled" property, which is the correct end-state for a series migrating the shader stack to Slang; the runtime A/B dispatch remains default-off, so the guard itself is still free when idle.

---

## Files

**Renamed:** `renderer/subsystems/SlangSpikeSubsystem.{h,cpp}` → `SlangParityGuard.{h,cpp}`; `renderer/settings/SlangSpikeSettings.h` → `SlangParitySettings.h`. Shader filenames kept (`slang_spike_gi.{comp,slang}`, `slang_spike_diff.comp`) to avoid orphaning committed `.meta` UUIDs.
**Edited:** `renderer/shader/SlangCompiler.{h,cpp}` (CompileReflectStage + PrepareModule), `renderer/shader/ShaderCompiler.{h,cpp}` (CompileStaged facade), `resources/importers/ShaderImporter.cpp` (CompileStaged), `resources/FileSystem.cpp` (`.slang`→Shader), `renderer/shader/ShaderWatcher.cpp` (`.slang` filter), `renderer/RenderPipeline.{h,cpp}` + `scene/systems/RenderingSystem.h` + `luthien/panels/RenderPanel.cpp` (rename + verdict UI), `core/Version.h` (3.2.1); shaders `slang_spike_gi.comp` + `slang_spike_gi.slang` (byte-identical camera ray).
**Deleted:** `renderer/subsystems` `RunLinkSpecCheck`; `assets/shaders/slang_spike_link.slang`.

---

## Verification

- **Build:** MSBuild Debug x64 clean after every commit (only the pre-existing `LNK4006` NULL_IMPORT_DESCRIPTOR + `Editor.cpp` C4244 warnings).
- **Offline shader compile (pinned SDK 1.4.341.1 — the engine's toolchain):** `glslc --target-env=vulkan1.3` on `slang_spike_gi.comp` and `slangc -profile spirv_1_5 -matrix-layout-column-major -fp-mode precise -stage compute -entry main` on `slang_spike_gi.slang` both succeed; `spirv-dis` of the Slang output confirms the four required caps + the `OpDecorate NonUniform` set the runtime scan asserts (12 at the engine's `-O none`; 14 under slangc's default opt — the count is optimization-dependent, so the gate checks presence). The lone slangc warning is the benign 41012 profile auto-upgrade the engine's session suppresses.
- **Runtime smoke (user):** the first pass invalidated the pixel-diff gate — TAA jitter made the numbers swing frame-to-frame and the verdict was view-dependent, and the ULP metric was pathological near zero (`maxUlp ≈ 9.6e8` at `maxAbsDiff ≈ 0.05`). That drove the rework to the deterministic SPIR-V gate. Re-smoke confirms: enable "Slang Parity Guard" → **SPIR-V guard: PASS**, `NonUniform 12`, `caps present` (stable, view/TAA-independent); cold boot compiles `gi.slang` via `ImportDirty` once with no error for the deleted link probe; editing `slang_spike_gi.slang` re-runs the scan + hot-reloads the Slang side; the pixel A/B is informational only.

---

## Hand-off / deferred (Phase 2+)

- **`optimization_level` meta knob** is ignored by both importers today (GLSL passes `optimize=false`), so SlangCompiler's fixed NONE+precise session is consistent — but production graphics `.slang` will want precise-off / opt-on. Plumb the knob through `CompileStaged` for both languages when Phase 2 ships real `.slang` shaders.
- **`VulkanShader::ToVkStage` returns `0`** for Raygen/Miss/etc. — fine for the Compute `gi.slang`, but graphics/RT `.slang` assets (Phase 2) need the full mapping.
- **`.slang` `import`/`#include` dependency tracking** isn't hashed into `State.json` — editing an imported header won't mark dependents dirty. Same gap GLSL has via `LuthIncluder`; Slang modules can fix it structurally later.
- **The shade ULP floor** (cofactor `Inverse3x3` vs builtin `inverse()`, `FetchHitSurface` `mul`-vs-`*`) lives in the shared geom_table GLSL path; unifying it would make the guard `maxUlp == 0` but touches every RT shader — left to the Phase 2 seam conversion.
