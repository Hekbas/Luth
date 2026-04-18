# v1.7.0 — arch-renderer-split

**Date:** 2026-04-18
**Commits:** 9
**Issue:** [#77](https://github.com/Hekbas/Luth/issues/77)

---

## Overview

Phase 3–4 of the architecture refactor. Dissolve the ~3 500-LOC `RenderingSystem` god-class (which lived in `scene/systems/` but was the de-facto renderer) into focused classes under `renderer/`. Consolidate scattered animation code into a new `animation/` module.

After this epic, `scene/systems/RenderingSystem` is a ~350-LOC ECS glue layer; all graphics resources (pipelines, descriptor sets, SPIR-V, UBOs, SSBOs, IBL maps, bloom textures, GPU timers, named-texture registry, preview textures) live on `RenderPipeline` in `renderer/`; animation data has its own top-level module.

See the multi-epic plan: [`docs/development/ARCH-REFACTOR-PLAN.md`](../../ARCH-REFACTOR-PLAN.md).

---

## Sub-Tasks

| # | Sub-task | Commit |
|---|---|---|
| A | Extract `FrameTargets` from `RenderingSystem` | `refactor(render): extract FrameTargets from RenderingSystem` |
| B | Extract `DrawListBuilder` | `refactor(render): extract DrawListBuilder from RenderingSystem` |
| C | Extract `LightGatherer` + `CascadeBuilder` | `refactor(render): extract light gathering + CSM cascade build` |
| D | Extract `RenderPipeline` (graph assembly) | `refactor(render): extract RenderPipeline graph assembly` |
| E1 | Thin `RenderingSystem` — init migration | `refactor(scene): thin RenderingSystem (init migration)` |
| E2 | Thin `RenderingSystem` — per-frame + debug migration | `refactor(scene): thin RenderingSystem (per-frame + debug migration)` |
| E3 | Thin `RenderingSystem` — field ownership migration | `refactor(scene): thin RenderingSystem (field ownership migration)` |
| — | Fix skybox reload on project change | `fix(editor): reload skybox on project change` |
| F | Consolidate `animation/` module | `refactor(animation): consolidate animation module` |

---

## Directory Changes

### New files
- `renderer/FrameTargets.{h,cpp}` — owns persistent scene textures (SceneColor/Depth, LDR, EntityID, Selection {mask,depth})
- `renderer/DrawListBuilder.{h,cpp}` — walks ECS once, partitions entities into opaque/cutout/transparent draw buckets
- `renderer/draw/DrawList.h` — bucket struct with tri-count summary
- `renderer/lighting/LightGatherer.{h,cpp}` — ECS → `LightUniforms` + shadow config
- `renderer/lighting/CascadeBuilder.{h,cpp}` — PSSM split + per-cascade ortho fit
- `renderer/RenderPipeline.{h,cpp}` — graph assembly + all graphics resources (~3 150 LOC)

### New folder
- `animation/` — houses `AnimationClip.h`, `Skeleton.h`, `BoneMatrixBuffer.{h,cpp}`, `AnimationController.h` (all moved via `git mv`, history preserved)

### Moved
- 5 files moved into `animation/` (from `renderer/` and `scene/`)
- 19 caller files bulk-rewrote their includes

### Added to `lighting/LightTypes.h`
- `DirectionalLightShadowParams` — per-frame shadow config from `Component::DirectionalLight`
- `CascadeData` — per-frame CSM output (4 matrices + split view-Z + texel sizes)

---

## Shrinkage

| File | Before | After | Δ |
|------|--------|-------|---|
| `scene/systems/RenderingSystem.cpp` | ~3 500 LOC | 348 LOC | **−90%** |
| `scene/systems/RenderingSystem.h` | ~490 LOC | 194 LOC | **−60%** |

The remaining `RenderingSystem` is the ECS glue layer the spec targeted: `Update()` orchestration, `UpdateLightUniforms()` (CPU-side gather + cascade build), mouse picking, editor-facing getters/setters, frame-debugger state, shader hot-reload dispatch. The ~100-LOC aspirational target was approached but not hit — `RenderingSystem` still holds `FrameTargets`, `DrawListBuilder`, `LightGatherer`, `CascadeBuilder`, `FrameDebugger`, `CameraParams`, and editor state, because those are all scene-level inputs per the spec's target shape.

---

## Key Design Decisions

### Bidirectional friend class kept
`friend class RenderPipeline;` on `RenderingSystem` allows `RenderPipeline` methods to read RS-side per-frame inputs (`CameraParams`, `ShadowParams`, `Cascades`, `FrameTargets`, `FrameDebugger`, `DrawList`, editor toggles) without widening the public API to ~25 getters. The coupling is intrinsic: Pipeline consumes scene state that by design lives on the ECS-glue layer. Dropping friend was an aspirational goal, not worth the verbosity. `RenderingSystem.h` drops most Vulkan includes as a result — only `VkSampler` (via `FrameDebugger`) and `VkImageView` (preview getter return types) remain.

### Sub-task D staging
Moving the entire graph-assembly chain + all 13 `Add*Pass` methods + `CollectSelectedHandles` + `CaptureSnapshot` was a 1 500-LOC mechanical move. Executed atomically via `perl` rewrite: `RenderingSystem::` → `RenderPipeline::` on class qualifiers, `m_X` → `m_System.m_X` on member accesses (since fields still lived on RS at that point). Friend class granted access. E1–E3 later inverted the perl rewrite for fields that migrated.

### Sub-task E sub-staged into E1/E2/E3
The "≤ 100 LOC" target in the spec required ~2 000 LOC of migration across 4 files — too risky for a single commit. Split into three atomic sub-commits:
- **E1** — init routines + ctor/dtor → `Pipeline::Initialize/Shutdown/OnResize`. `RenderingSystem.cpp`: 3 073 → 1 194 LOC.
- **E2** — per-frame `Update*` helpers + `BuildGPUObjectBuffer` + debug blit/preview helpers moved. 1 194 → 324 LOC.
- **E3** — field ownership migration. ~40 graphics fields moved from RS to Pipeline; pass files bulk-rewrote `m_System.m_X` → `m_X`. RS-retained fields kept their `m_System.` prefix via negative-lookbehind perl.

### `k_MaxGPUObjects` + indirect-region constants moved
The `static constexpr u32 k_MaxGPUObjects = 4096;` constant (plus `k_IndirectRegionCount` / `k_IndirectRegionStride`) migrated from `RenderingSystem::` private statics to `RenderPipeline::` public statics in E3. Pass files consume them via `RenderPipeline::k_MaxGPUObjects`.

### `animation/` module carved out (sub-task F)
`AnimationClip`, `Skeleton`, `BoneMatrixBuffer` (data + GPU buffer) moved from `renderer/`. `AnimationController` (blend controller) moved from `scene/`. `AnimationSystem` stayed in `scene/systems/` because it walks `Component::Animation` + `Component::BoneAttachment` — ECS territory by design.

---

## Skybox Init Bugfix

Partway through E3 verification, the skybox rendered black at startup until the user manually reloaded it via the editor. Root cause: `RenderingSystem::ctor` runs during `App::App` *before* any project loads. `FileSystem::ResolveAsset("textures/environment.hdr")` falls back to engine assets (`s_HasProject = false`), but the engine ships no HDR — only `samples/assets/textures/environment.hdr` exists. `IBL::Precompute` hits its fallback path, returns 1×1 placeholders without skybox SPIR-V, and `CreatePipelines` silently skips the skybox pipeline. Likely pre-existing but surfaced during refactor verification.

**Fix:** `Editor::OnProjectChanged` (tail of `App::LoadProject`) now re-resolves the settings' `skyboxPath` against the freshly set project root and calls `ReloadSkybox` if the resolved path exists.

---

## Lessons

**Scope ambition vs. commit granularity.** The spec's "≤ 100 LOC" target for `RenderingSystem.cpp` was the right aspiration but couldn't be delivered atomically. The epic spec itself suggested sub-staging D into D1/D2/D3 if needed; E adopted the same pattern. Per-commit build verification is non-negotiable; the single-commit target would have been a multi-day breakage risk.

**Perl lookbehind saves double-rewrites.** Each sub-task's bulk rewrite had to avoid re-rewriting already-prefixed accesses. `(?<!m_System\.)\bm_X\b` is the pattern — it runs idempotently over a file that's been partially rewritten, so repeat runs are safe.

**Refactor verification surfaces pre-existing bugs.** The skybox-black issue likely existed before the epic — the refactor just put eyes on it. Worth a post-epic pass on anything that looks "working" but might have a similar latent flaw.

**File moves with `git mv` preserve blame.** Sub-task F's 5-file move kept `git log --follow` history intact; the diff showed 0-line changes on the moved files themselves. Trivial but important for ongoing code archaeology.

---

## Build Verification

- Debug x64 builds clean after every sub-task (9 incremental builds)
- Premake regeneration clean on each sub-task
- No new warnings (only pre-existing C4267 / LNK4006 noise)
- `Luth.lib` + `Luthien.exe` artifacts produced

### Runtime verification (user-confirmed)
- A: FrameTargets resize + render-pass parity ✅
- B: tri-count + opaque/cutout/transparent ordering preserved ✅
- C: CSM cascades split identically ✅
- D: full visual parity + Frame Debugger works ✅
- E1/E2/E3: startup, shutdown, resize, hot-reload, picking, capture ✅
- F: skinned mesh animation + bone debug overlay ✅
- Skybox fix: loads correctly when project opens ✅
