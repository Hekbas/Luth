# rt-renderer.D.2 — volumetric-rt-shadows

**Date:** 2026-06-09
**Commits:** on `feat/volumetric-rt-shadows` (S0 `5df3bd7`, S1 `7c357df`, review-fix `7714bc1`, wrap-up) + two folded
post-D.1 audit fixes (`0db80bf`, `c34fc86`)
**Issue:** [#150](https://github.com/Hekbas/Luth/issues/150)
**Umbrella:** [#127](https://github.com/Hekbas/Luth/issues/127)
**Series:** `rt-renderer`, Phase D. Mode A series-coalesced, **v3.0.18** tag-only, no Release.

---

## Overview

Per-froxel **RT shadow rays** in the volumetric fog inject-scatter pass, so **point lights + arbitrary
occluders** cast fog shadows. Before this, only the sun was shadowed in the fog (via CSM in
`volumetric_inject_scatter.comp`); cluster point lights were unshadowed there, and the sun's fog CSM was the
Phase-A.4 placeholder. With the toggle on, each froxel casts one shadow ray toward each in-range cluster
point light AND a ray toward the sun (replacing `SampleCSM`); off, the path is byte-identical to before.

The volumetric system was already architecturally ready: the inject-scatter pass reconstructs each froxel's
`worldPos` (Wronski log-Z), runs on AsyncCompute, and binds Set 0 (TLAS at b6, COMPUTE-stage). So this was a
shader change + an inline AS barrier + a `needTlas` gate + a toggle — **no new descriptor, layout, pool, or
per-view resource.** The `Visible()` rayQuery helper is the same shape as `rt_reflections.comp`'s.

**No denoiser** — the fog is low-frequency, trilinearly froxel-filtered, AND already temporally accumulated
by the existing `VolumetricResolve` pass (`temporalAlpha≈0.05` + Karis 3×3×3 clamp). 1-spp hard rays suffice.
This is the key simplification vs. the surface DI/GI/reflection paths, all of which needed SVGF.

One adversarial-review workflow ran against the committed S0 (AS/sync + pass-ordering + the shader sign chain
+ the Wronski contract interaction).

---

## Sub-tasks

| # | What landed | Commit |
|---|---|---|
| S0 | **Shader RT rays + AS barrier + TLAS-build ordering hoist.** `volumetric_inject_scatter.comp` `450→460` + `GL_EXT_ray_query`; `accelerationStructureEXT topLevelAS` at Set 0 b6; the `Visible()` helper; `rtFog = volScatterParams.z > 0.5`; sun swap (`Visible(-dirLight.direction)` vs `SampleCSM`) keeping `sunAbs` in both modes; point-light wrap (`Visible(-incident)` inside the range cull). Inline AS-build→COMPUTE barrier in `AddInjectScatterPass` execute, gated on `IsRtShadowsEnabled`. `RenderPipeline.cpp` hoists the TLAS-build block **before** the volumetric chain. `VolumetricSettings.rtShadows` + `GlobalSubsystem` `volScatterParams.z` fill. | `5df3bd7` |
| S1 | **Editor toggle.** `RenderPanel.cpp` "RT Shadows" checkbox + perf tooltip in the Volumetric In-Scatter section. (The field / fill / `IsRtShadowsEnabled` / `needTlas` gate consolidated into S0.) | `7c357df` |
| review-fix | **`needTlas` tightening (S0 review nit).** The volumetric term now requires `volumetricEnabled`, not just `rtShadows`, so a fog-off view doesn't build a TLAS the (then-unregistered) scatter pass would never read. | `7714bc1` |

---

## Design decisions

### THE LOAD-BEARING FIX — pass ordering (the plan agent's catch)
`AddInjectScatterPass` registers inside the volumetric block; `AddTlasBuildPass` was registered **after** it.
Passes execute in registration order on the shared AsyncCompute primary; the inline AS barrier gives memory
*visibility* but NOT execution *ordering*. So the scatter would have read an empty/stale TLAS → **silent
no-shadows** (no crash, no VUID). Fix: hoist the `runRtShadows`/`needTlas`/`AddTlasBuildPass` block to BEFORE
the volumetric block. The TLAS build is self-contained (`SetHasSideEffect`, no RG resource deps), so it moves
freely. The S0 review re-confirmed this RG does not reorder compute passes (`RenderGraph::Execute` emits in
pass-index order; Phase-1 parallelism is graphics secondary-buffer recording only).

### Wronski contract — no change, no double-count
The RT visibility multiplies the **per-light L term inside J** (`scat = albedo·J`, `J = Σ phase·L·visibility
+ ambient`) — the same `visibility ∈ [0,1]` factor that was `SampleCSM` (sun) / implicit `1.0` (points). It
does NOT touch density/σ_t, integrate's `(1−exp(−σ_t·dt))`, the `albedo=σ_s/σ_t` premultiply, the
multi-scatter ambient (visibility-independent by design — survives in shadow), or the `scatteringIntensity`
artistic knob. No contract / integrate / resolve / composite change.

### Sun keeps both RT visibility AND `sunAbs`
The sun's RT ray tests **opaque-occluder** visibility; the existing `SunFogTransmittance` density-march is
**in-medium extinction** (fog self-shadowing along the light path). Physically distinct — both stay. The
density-march is unchanged and runs in both CSM and RT modes.

### Free-space ray origin (`offsetN = vec3(0)`)
The froxel center is free space — no surface to self-hit, so no shading-normal offset is needed; the `dir*sMin`
nudge suffices. A froxel inside an occluder reports shadowed but is visually inert. Point-ray `maxDist =
dist − 2·sMin` stops just short of the light; sun-ray `maxDist = 1e30`.

### Toggle via `volScatterParams.z` — no new UBO field
D.1's lesson: a new `GlobalUniforms` field desyncs the inline-`GlobalUniforms`-prefix shaders (skybox.frag
etc.) that don't `#include globals.glsl`. So the gate rides a free `volScatterParams.z` slot, filled by
`GlobalSubsystem` from `VolumetricSettings.rtShadows`. The barrier-gate (`IsRtShadowsEnabled`) and the
shader's `rtFog` uniform both derive from the same `rtShadows` flag — perfectly coupled, no stale-TLAS-read
path when off.

---

## Bugs caught (adversarial verification)

- **S0 review — no correctness fix; one efficiency nit folded.** Two reviewers (conf 0.92 / 0.95) flagged a
  point-light ray-direction inversion at lines 228/232 as "high severity." **False positive** — they misread
  the sign chain: `toLight = position − worldPos` is froxel→light, so `incident = −toLight/dist` is
  light→froxel (matching the dir-light `incident` convention used for the HG phase `dot(incident,
  −viewDirWorld)`), and `Visible(worldPos, −incident, …)` traces froxel→light = **toward the light** (the
  correct occlusion test). Applying their suggested `Visible(worldPos, incident, …)` would have *introduced*
  the inversion. The sun ray `Visible(−dirLight.direction)` is likewise correct (the reviewers conceded this).
  Verified by hand against the committed shader before rejecting. The one legitimate finding was LOW-severity:
  `needTlas` built a TLAS for the volumetric term even when the view's fog was disabled — folded (`7714bc1`).
- **False positives correctly rejected:** the RG-reorders-compute-passes claim (this RG preserves pass order),
  the "barrier must always fire" claim (barrier-gate and `rtFog` derive from one flag → no stale read when
  off), and any cross-queue / QUEUE_FAMILY / semaphore complaint (both passes are AsyncCompute on the same
  primary; intra-queue order + the memory barrier suffice).

---

## Folded audit fixes (post-D.1)

Two pre-existing ReSTIR-DI bugs surfaced by a deep code-review pass land in this version range (per the choice
not to tag them standalone):

- `0db80bf` **fix(renderer): DI prev reservoir cross-frame barrier** — the previous-frame DI reservoir read
  lacked a cross-frame barrier.
- `c34fc86` **docs(shaders): correct DI remod contract + emissive UV note** — corrected the DI remodulation
  contract wording + an emissive-UV note.

---

## Files touched

**Engine — modified:** `assets/shaders/volumetric_inject_scatter.comp` (RT rays + `Visible()` + sun swap +
point wrap), `subsystems/VolumetricSubsystem.{h,cpp}` (`IsRtShadowsEnabled` + AS barrier),
`renderer/RenderPipeline.cpp` (TLAS-build hoist + `needTlas` gate), `subsystems/GlobalSubsystem.cpp`
(`volScatterParams.z` fill), `settings/VolumetricSettings.h` (`rtShadows`).
**Editor:** `panels/RenderPanel.cpp` ("RT Shadows" toggle).

---

## Verification

Build clean (Debug x64, luth → luthien → Runtime), no new warnings; `volumetric_inject_scatter.comp` passes
`glslc --target-env=vulkan1.3` (460 + ray_query). One adversarial review workflow against committed S0 (AS/sync
+ ordering + shader sign chain + contract) — one efficiency nit folded, all "high" findings verified as false
positives. **Runtime smoke-test before merge** (visible UX): enable fog + RT Shadows; point lights cast fog
shadows behind occluders; sun fog shadow from RT; toggle off = unchanged; FrameDebugger confirms TlasBuild
precedes the scatter dispatch; `LUTH_VALIDATION` clean.

---

## Hand-off / deferred

- **Cost (HIGH at High preset):** ~6.22 M froxels × (cluster-point-count + 1 sun) rays — gated by the
  default-off toggle. Deferred mitigations if the showcase needs them: froxel-z max ray distance, half-res
  shadow froxels, a sun-only RT tier.
- **1-spp, no denoiser** — relies on the existing temporal resolve. If point-light fog shadows shimmer under
  motion, the escalation hatch is a froxel-space spatial blur or a dedicated low-res shadow accumulation;
  not expected given the trilinear froxel filtering.
- Git hooks not installed in this workspace — comment policy honoured manually.
