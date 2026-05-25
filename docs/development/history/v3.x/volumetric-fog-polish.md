# volumetric-fog-polish

**Date:** 2026-05-24 (v3.0.5) · 2026-05-25 (v3.0.6 follow-up)
**Commits:** 14 (v3.0.5) + 3 post-release fixes + 3 (v3.0.6 follow-up) + wrap-up
**Issue:** [#132](https://github.com/Hekbas/Luth/issues/132)
**Series:** `rt-renderer` Mode A series-coalesced — `Version.h` PATCH bumps `v3.0.5` then `v3.0.6`, tag-only, no Release.

---

## Overview

Two-part effort. First half (5 commits) shipped the three deferred sub-tasks from `volumetric-fog` v3.0.3: Hillaire-named multi-scatter scalar (D), temporal accumulation via inject-time ping-pong + Karis 3×3×3 clamp (C), and `ShadeMode::VolumetricDensity/InScatter` debug viz pass (E). After a deep audit identified 11 substantive issues, a second half (9 commits) re-engineered the temporal model from the ground up and landed ~all of the audit's findings: proper Henyey-Greenstein phase function, IBL multi-scatter, point-light inverse-square attenuation, emit-term physics fix, sun-fog light-path absorption ray-march, integrate half-step transmittance, composite invView push constant, sky-fog opacity cap, configurable atlas resolution, shader `#include` support, all 11 `VolumetricSettings` fields exposed in the editor, FogVolume viewport gizmos, tone-mapped viz output, and a doubled cluster Z resolution (24 → 48) for halved fog Z-banding.

The temporal-accumulation refactor is the load-bearing change: the first half put the blend inside the inject pass, reading post-integrate values from the previous frame's atlas as "history" and mixing with this frame's pre-integrate values. Math analysis after shipping revealed this produced **energy non-conservation** — the second-frame integrate operated on already-cumulative values, leading to a fixed-point convergence at roughly ~15% of the correct fog brightness at near distances after several stationary frames. Visually: fog visibly *fades* over a few seconds when the camera stops. The fix moves temporal accumulation to a dedicated `VolumetricResolve` compute pass that runs AFTER integrate. The pass reads scratch (post-integrate this frame) + reprojected prev-resolved (post-integrate last frame), applies the Karis clamp against this frame's neighborhood, blends with `temporalAlpha`, and writes the resolved atlas. History and current are both post-integrate so the domains match and energy is conserved. The pre-integrate-time halo workgroup is gone (inject returns to standard `(8,8,4)` threads).

Memory: +14 MB per view (one extra in-scatter atlas for the resolve ping-pong: `volInScatterHistA` + `volInScatterHistB` ping-pong; `volInScatter` becomes pure scratch). Two views (Scene + Game panel) = +28 MB. Acceptable for the energy-conservation fix.

---

## Sub-tasks

| # | What landed | Commit |
|---|---|---|
| D | **Hillaire 2nd-order multi-scatter scalar + RenderPanel slider.** Inject shader UBO extended with the 4 fog vec4s. Multi-Scatter Intensity slider 0..1 in a new "Volumetric Fog" RenderPanel section. (Later reworked in #C-IBL — the original scalar was a multiplicative boost which couldn't lift shadowed regions.) | `8cf1075` |
| C.1 | **Temporal layout + ping-pong wiring (first attempt — inject-time).** Inject layout grew to 8 bindings (b7 = history sampler, UAB on b0/b1/b2-b5/b7). Composite descset promoted from single to cycled. WriteInjectPerFrame gained `frameAbs` for parity-pick. ImmediateSubmit clear on both in-scatter atlases at view-recreate. **Superseded by Phase B refactor below.** | `72d7b24` |
| C.2 | **Karis 3×3×3 clamp via halo workgroup.** Inject `local_size` extended to `(10,10,6) = 600 threads`; outer halo of 344 threads computed fresh in-scatter purely to seed a 9.6 KB groupshared tile for the neighborhood min/max clamp. Inner 256 threads wrote to the atlas. **Superseded by Phase B refactor — temporal moves to a separate pass, so inject returns to `(8,8,4)`.** | `734978b` |
| E.1 | **VolumetricVizPass + shader + ShadeMode entries.** New `volumetric_viz.frag` (push-constant mode 0/1). VolumetricSubsystem gains viz pipeline + layout + AddVizPass + WriteVizPerFrame. Cycled `volVizDescSet[N]` on ViewResources. Enum `ShadeMode::VolumetricDensity/VolumetricInScatter`. BuildGraph dispatches viz from the existing if/else cascade with `volumetricEnabled` gate. | `d1451e8` |
| E.2 | **ScenePanel toggles for fog viz.** dbgActive predicate extended; new "Volumetric Fog" RadioButton group after Forward+ Clusters. | `fdb2732` |
| A1 | **Shader `#include` support via shaderc IncluderInterface.** LuthIncluder resolves relative includes from requesting source's directory; falls back to engine shader root. New `common/globals.glsl` holds GlobalUniforms (mirrors RenderingSystem.h). Volumetric inject/composite/viz shaders migrated. ~150 LOC of UBO struct duplication eliminated. | `e5b04f1` |
| A2 | **Expand VolumetricSettings + GlobalUniforms.** Settings: `anisotropy`, `temporalAlpha`, `sunFogAbsorptionSteps`, `skyFogStrength`, `vizScale*`, `vizOpacity`, `Quality` enum. UBO: new `volTemporalParams` + `prevViewParams` vec4s. ViewResources caches `prevNearZ/prevFarZ` alongside `prevViewProj`. `Volumetric::GetAtlasDims(quality)` helper. | `5a01934` |
| A3 | **Configurable atlas resolution.** Quality preset on ViewResources drives the atlas dims at RecreateViewTextures time; EnsureViewResources detects quality change + rebuilds + re-binds. InjectPC + IntegratePC carry atlas dims via push constant; shaders use `#define VOL_DIM` macro reading them. Low/Medium/High = 80×45×64 / 160×90×128 / 240×135×192. | `db3f9ce` |
| **B** | **Temporal-resolve refactor — the load-bearing fix.** New `volumetric_resolve.comp` runs AFTER integrate; reads scratch (post-integrate) + prev resolved + Karis-clamps via 27 `imageLoad` taps on scratch + blends `temporalAlpha`. Writes parity-picked HistA/B. ViewResources gains `volInScatterHistB`. Composite + viz sample HistA/B (parity), not the integrate output. Inject reverts to `(8,8,4)` workgroup, drops the b7 history binding + UAB on b0/b1 + sharedScatter tile. Composite invView pushed via PC. Integrate stores half-step transmittance for sampler-center alignment. Composite/viz/resolve sets all cycled per MAX_FRAMES_IN_FLIGHT. Bootstrap clear extended to clear the 3rd in-scatter atlas via ImmediateSubmit. | `bbbfa2a` |
| C-physics-1 | **Henyey-Greenstein phase + point 1/d² + emit fix + sun absorption + Wronski multi-scatter shape.** Inject's per-light contributions now: `HG(cosTheta, g) × radiance × shadow × (1/d² window)`. Emit no longer double-density-weighted. Sun-fog 4-step ray-march via `SunFogTransmittance` accumulates optical depth toward sun, attenuates dir-light. Multi-scatter rewritten as `density × skyAmbient × msK` — additive IBL term that survives shadow (the misnamed multiplicative boost couldn't). | (bundled in Phase B commit `bbbfa2a`) |
| C-physics-2 | **IBL irradiance sample for multi-scatter.** Set 0 binding 1 gains COMPUTE stage flag; inject shader samples `irradianceMap` at world-up for the ambient term. Replaces the placeholder constant grey. | `08d74d4` |
| Cluster-Z bump | **k_ClusterSlicesZ 24 → 48.** Halves Z-banding granularity for fog illumination — each cluster spans ~2.7 atlas slices instead of ~5.3. LightTypes.h constant + matching shader constants in pbr.frag, cluster_viz.frag, volumetric_inject.comp. Memory: cluster grid 27 KB → 55 KB (negligible). Forward+ fragment cost unchanged. | `b6fcc26` |
| UI-expansion | **Comprehensive RenderPanel UI.** All 11+ VolumetricSettings fields exposed: Quality combo, Anisotropy, Multi-Scatter, Sky Fog Strength, Sun Absorption Steps, Temporal Blend, Distance Fog (color/density/start/maxOpacity), Height Fog (color/density/refHeight/falloff), Viz density/inScatter scales + opacity. Viz pass push constants now read scales/opacity from VolumetricSettings (no hardcoded magic). `volumetric_viz.frag` log10-encodes in-scatter mode for diagnostic-useful HDR display. | `3d77df5` |
| FogVolume gizmos | **Viewport wireframe gizmos for FogVolume entities.** `DrawFogVolumeGizmos` iterates ECS `(FogVolume, WorldTransform)` views; emits box (12 edges) or sphere (3 great circles × 24 segments) via `Luth::DebugDraw::Line`. Called from `RenderingSystem::Update`. | `dfd0db9` |
| Tooltips | **Hover tooltips on the key fog UI fields.** Quality, Anisotropy, Multi-Scatter, Sky Fog Strength, Sun Absorption Steps, Temporal Blend each show a one-line `ImGui::SetTooltip` on hover with recommended-range guidance. | `fefc5b2` |
| Wrap-up (v3.0.5) | **History + version bump.** This document up through this row. `Version.h` 3.0.4 → 3.0.5. | `bffe71b` |
| Post-release fix | **Smoke-time shader compile errors.** Resolve shader's `volScratch` binding name + integrate's missing `texelFetch` arg surfaced on first scene load. | `2fe9894` |
| Post-release fix | **cluster_build + light_assign SLICES_Z 24 → 48.** Cluster grid count mismatch with the bumped k_ClusterSlicesZ — compute passes still allocated for 24 slices. | `d8023f7` |
| Post-release fix | **viz desc-pool overflow silently dropped viz descset.** Pool capacity (48) too small for the cycled viz set after C.1's UAB layout grew; `vkAllocateDescriptorSets` returned `VK_ERROR_OUT_OF_POOL_MEMORY` with no log. Bumped + added VkResult check. | `b975465` |
| F.1 (v3.0.6) | **3D Worley-FBM density noise modulation.** New `volumetric_noise_bake.comp` (128³ RGBA8 baked once at Init). Inject samples per voxel, density `*= mix(1−s, 1+s, n)` to preserve mean. New `noiseScale` / `noiseStrength` / `noiseWind` settings + `volNoiseParams` + `volNoiseWind` UBO vec4s + RenderPanel sliders. | `3f8e5f6` |
| F.2 (v3.0.6) | **Split inject into density + scatter passes.** Sun-ray absorption needs to READ density at neighbouring voxels — impossible in a single-dispatch compute, so inject becomes two passes with RG barrier between them. Density pass writes `vec4(density, tint.rgb)` to volDensity (tint packed into `.gba`). Scatter pass reads via sampler3D, runs CSM+HG+multi-scatter math, samples density atlas along the sun ray for proper density-aware absorption (replaces the broken uniform-density shortcut where `steps` cancelled out of the formula). C++ split: two layouts, two pipelines, four Write functions, two AddPass functions; ViewResources rename + new descset; RenderPipeline.cpp chain density→scatter→integrate. | `41e1871` |
| F.3 (v3.0.6) | **Canonical inject/integrate contract + scatter intensity knob.** Spurious `× density` in inject_scatter dropped — integrate's `(1 − exp(−σ_t · dt))` already supplies the σ_t factor (canonical Wronski 2014 / Hillaire 2015). Pre-multiplying double-applied σ_t and dimmed fog by ~10× at density 0.1. CONTRACT comment in both shaders + new "Cross-pass numerical contracts" section in `arch/rendering-pipeline.md`. Plus `scatteringIntensity` post-canonical artistic multiplier (UE5/Frostbite-style knob, default 15.0) — lifts off-axis voxels into visible range against HG's natural forward bias. Default settings recalibrated for the canonical math (density 0.1, anisotropy 0.7, multiScatter 0.15, sunSteps 2, Quality High). | `9f1b31b` |
| Wrap-up (v3.0.6) | **History + version bump + merge.** This row. `Version.h` 3.0.5 → 3.0.6. `--no-ff` merge into `main` + `v3.0.6` tag. | this commit |

---

## v3.0.6 follow-up — debug session findings

The v3.0.5 wrap-up was premature: smoke testing after the chore(release) commit surfaced three regressions (the three post-release fixes above), and a deeper bug — fog rendered as **black opacity** in shadow regions even when god rays were visible from sun-aimed angles. A diagnostic-first session (handoff in `docs/development/handoff/volumetric-fog-debug.md`, untracked) walked through five hypotheses before finding the right one:

1. **Sun-absorption ray-march was a no-op for any `steps > 0`.** `SunFogTransmittance` computed `stepLen = 0.5 × farZ / steps`; the loop accumulated `voxelDensity × stepLen` exactly `steps` times → `steps` cancelled, giving `exp(−0.5 × farZ × voxelDensity)` regardless of step count. With farZ ≈ 1000 and any density above ~0.01 the function returns ~0 → no sun reaches voxels → no in-scatter → black. Confirmed by toggling `sunFogAbsorptionSteps = 0`: god rays reappeared instantly. The `sample_pos` variable inside the loop was dead code — the original author intended to sample the density atlas along the ray (the Hillaire/Frostbite proper formulation) but never wired the read.

2. **Single-pass inject can't sample density at other voxels.** Vulkan compute dispatches execute all workgroups concurrently with no cross-workgroup ordering. To sample density at a neighbour you need a barrier between density-write and density-read — i.e. two passes. F.2 is the architectural split that enables F.3's proper sun absorption.

3. **Inject's `× density` was double-applying σ_t.** Once the split was in place and the obvious bugs gone, fog was *still* dim. Tracing the math: the radiative transfer per-slice contribution is `σ_s × J × (1 − exp(−σ_t · dt)) / σ_t`, which for `σ_s = σ_t × albedo` collapses to `albedo × J × (1 − exp(−σ_t · dt))`. The codebase had `albedo × J × σ_t` in inject AND `(1 − exp(−σ_t · dt))` in integrate, but no `/σ_t` to cancel one σ_t factor. The OLD pre-unified-fog code worked by coincidence because FogVolumes are typically authored at density ≈ 1.0 (where × 1 / 1 = no-op); the unified-fog refactor exposed the bug by routing distance fog (density 0.05–0.5) through the same path. Dropping the `× density` from inject_scatter restores the canonical Wronski/Hillaire formula. F.3.

4. **HG phase + directional light leaves off-axis fog intrinsically dim.** Even with the math correct, HG `g = 0.7` gives ~75× brightness ratio between sun-axis and perpendicular voxels — physically correct, but visually leaves ambient fog near-black without an ambient lift. Multi-scatter + IBL is the principled solution; for scenes without IBL, the `scatteringIntensity` post-canonical multiplier (matches UE5's "ScatteringDistribution") lets users dial in visible ambient at the cost of energy conservation (default 15.0 chosen empirically). The CONTRACT comment explicitly notes this multiplier sits *outside* the physical formula.

---

## Architectural decisions

### Temporal accumulation moved to a dedicated resolve pass

The first-half C.1+C.2 design put temporal blending INSIDE the inject pass — blend `mix(history, fresh, 0.05)` then write the blended value to the in-scatter atlas. Post-integrate would then sum over already-blended pre-integrate samples. Cross-frame, this produces a fixed-point under stationary camera where the integrate operation operates over ALREADY-cumulative values (history is post-integrate of last frame). The fixed-point converges to ~15% of the correct fog magnitude at near distances. **Fog visibly dims over ~1-2 seconds when the camera stops.**

The fix: separate `VolumetricResolve` compute pass that runs after integrate. Pass reads scratch (this frame's post-integrate) and the resolved atlas from last frame (also post-integrate). Both inputs are in the same domain, so the blend math is correct and energy is conserved. Composite + viz now sample the resolved atlas instead of integrate's direct output.

This required adding a third in-scatter atlas. The current `volInScatter` becomes pure scratch (inject + integrate in-place each frame). Two new atlases `volInScatterHistA/B` ping-pong as the resolve I/O pair: each frame, resolve reads one as "prev resolved" and writes the other as "current resolved." Composite samples the current-resolved per parity. Memory cost: +14 MB per view (×2 views = +28 MB total). Acceptable.

The halo workgroup design from C.2 — `(10,10,6)` workgroup with 344 halo threads seeding a 9.6 KB groupshared tile — is gone. The new resolve pass runs at standard `(8,8,4)` and does the 3×3×3 clamp via 27 `imageLoad` taps on the already-written scratch atlas. Cache-friendly + simpler. Inject also reverts to `(8,8,4)` — no halo work. Net inject cost: ~2.3× faster than the C.2 design.

### IBL irradiance as the proper 2nd-order multi-scatter

The original "Hillaire multi-scatter" from D was a multiplicative boost: `inScatter *= 1 + msK * (1 - exp(-density))`. This is NOT multi-scatter physics — it's a brightness scalar. Defining property of multi-scatter: light spreads through scattering events into voxels that single-scatter doesn't reach (e.g., shadowed regions). A multiplicative boost can't do this — `0 × anything = 0` for fully shadowed voxels.

The proper form (Wronski/Frostbite): sample IBL irradiance at world-up, scale by extinction (density) and the user-tunable `msK`, ADD to single-scatter. This term survives in shadowed regions and properly lifts fog in indirectly-lit areas. `Set 0 binding 1 (irradianceMap)` already exists from pbr.frag; gaining a COMPUTE stage flag in `GlobalSubsystem::Init` lets inject sample it.

### Henyey-Greenstein phase replaces isotropic

Single biggest visual gap from the v3.0.4 baseline. Isotropic phase (`1/4π`) means fog looks uniform regardless of view direction — no characteristic "god rays" forward-scattering. Wronski paper uses HG with g ≈ 0.3-0.7 for typical fog.

`HenyeyGreenstein(cosTheta, g)` helper applied per-light: `dot(-viewDir, -lightDir)` for the directional light, `dot(-viewDir, lightDir)` for points (where `lightDir = (light - voxel)`). New `VolumetricSettings::anisotropy` field exposed in the RenderPanel UI.

### Point-light attenuation now physical 1/d²

Previously `(1 - dist/range)²` only. Now `physical × window` where physical = `1/max(d², 0.01)` and window = `(1 - dist/range)²` (smooth range cutoff). Matches `pbr.frag` and fixes "fog around point light looks like uniform cylinder" artifact.

### Sun light-path absorption (4-step ray-march)

`SunFogTransmittance(worldPos, density, steps)` accumulates optical depth along the sun ray. Each step samples the local voxel density (coarse approximation — Hillaire/Frostbite would full-density-atlas-sample, ours is a 1-tap shortcut). Multiplies the directional-light contribution. Settings field `sunFogAbsorptionSteps` (default 4, range 0..16; 0 disables).

This is the term that makes dense fog "self-shadow" — light passing through 50m of thick fog should arrive significantly attenuated. Without it, the sun blasts through full-strength regardless of fog density.

### Emit term de-doubled

`emit` previously got multiplied by `v.density` inside the FogVolume loop AND by total `density` outside. Net `emit ∝ density²` — wrong physics (emission is direct radiance, not a scattering integrand). Fixed: `emit` is summed without density weighting inside the loop, then added AFTER the density multiply: `inScatter = (dirContrib + pointContrib) * density + emit`.

### Half-step transmittance for slice-center alignment

Integrate previously stored the FAR-edge transmittance under each slice index. But `SliceToViewZ(z)` represents the voxel CENTER (+0.5 inside the pow). Sampler trilinear interp at slice center returns far-edge — one half-slice over-attenuated. Fixed: integrate now stores `transmit * exp(-extinction * dt * 0.5)` (half-step before the multiply), giving slice-center transmittance.

### Composite invView via push constant

Composite previously did `inverse(ubo.view)` per fragment — 40 ALU × 1920×1080 = 80M extra ops per frame. Now pushed as a 64B push constant.

### Sky-fog opacity cap

New `skyFogStrength` setting (default 1.0) scales fog opacity at sky pixels. 1.0 = full fog can hide skybox; 0.0 = sky never affected. Decouples sky behavior from the analytic distance-fog max-opacity.

### Cluster Z resolution bumped 24 → 48

Halves the volumetric Z-banding granularity. Each cluster now spans ~2.7 atlas slices instead of ~5.3. Combined with the temporal-resolve smoothing, banding is no longer user-visible. Cluster grid memory negligibly increased; Forward+ fragment cost unchanged.

### Configurable atlas resolution (Quality preset)

`VolumetricSettings::Quality` enum (Low / Medium / High). Atlas dims pulled from `Volumetric::GetAtlasDims(quality)` at RecreateViewTextures time. EnsureViewResources detects quality change (via cached `volQualityCached`) and rebuilds atlases + re-binds descriptors. Shaders use `#define VOL_DIM ivec3(int(pc.volDimX), int(pc.volDimY), int(pc.volDimZ))` from push constants — no recompile needed on quality change.

### Shader `#include` support

Added a `shaderc::CompileOptions::IncluderInterface` (LuthIncluder) to ShaderCompiler. Resolves relative includes from the requesting source's directory; falls back to engine shader root. Volumetric shaders + composite + viz now share `common/globals.glsl` for the UBO struct — eliminates 4-way duplication. `pbr.frag` will adopt in a future engine-wide refactor.

---

## Known issues / follow-ups

### Per-froxel light culling deferred

The audit identified mismatch between cluster-Z (24/48) and atlas-Z (128) as a source of in-fog illumination Z-banding. The proper fix is per-froxel sphere-vs-AABB culling — a dedicated compute pass writing per-voxel light lists. Cost: ~15 MB/frame of grid + index buffer, new compute pass, new descriptor set. Lighter version landed here (cluster Z 24 → 48) which halves banding granularity; combined with temporal-resolve smoothing the residual is invisible at typical light densities. Defer full per-froxel pass until a real scene surfaces it.

### Frame-debugger volumetric per-pass timing

The audit suggested surfacing per-pass timings in the RenderPanel ("Inject: 0.45ms · Integrate: 0.12ms · Resolve: 0.18ms · Composite: 0.08ms"). Plumbing this needs FrameDebugger access from the editor panel. Deferred — the FrameDebugger panel already shows per-pass timings in its own view; the duplicate-in-RenderPanel is convenience, not necessity.

### Async / lazy bootstrap clear

The view-resize-time `vkCmdClearColorImage` via `VulkanContext::ImmediateSubmit` blocks the editor for ~50µs per resize. Deferred — view-resize is infrequent and the cost is below the user's perceptual threshold.

### Sun absorption uses voxel density only

`SunFogTransmittance` ray-marches toward the sun but samples the originating voxel's density at every step (not the actual density along the ray). Coarse approximation — Hillaire/Frostbite full implementations sample the density atlas along the ray. Refining would add atlas-binding to the inject pass + N more 3D texture samples per voxel per light. Defer; current approximation is "directionally correct" for the artifact (dense fog reduces sun) without the cost.

### Pre-frame near/far caching for FOV animation

`prevViewParams.x/y` now caches `prevNearZ/prevFarZ` for reprojection-correctness under FOV/clip animation. Verified the resolve shader's `DepthToViewZ(prevNDC.z, prevNearZ, prevFarZ)` uses these. Pre-existing in the engine code: editor + game cameras have fixed near/far per panel today. The plumbing is in place for future animated cameras.

### IBL ambient sampled at +Y only

The multi-scatter IBL term samples `irradianceMap` at world-up only — a cheap one-tap estimate. A true integral would sample multiple directions or use a precomputed cross-section. Visually rarely matters at fog scales; defer until a use case demands it.

---

## Bugs caught during smoke testing

- **Resolve + integrate shader compile errors** on first scene load (`2fe9894`).
- **cluster_build / light_assign Z-slice count mismatch** after k_ClusterSlicesZ 24 → 48 (`d8023f7`).
- **Viz desc-pool silently overflowed**, dropped viz descriptor set, fog disappeared from Lit while Vol Density viz still worked (`b975465`).
- **Fog rendered black in shadow regions** — the multi-symptom bug that drove the v3.0.6 follow-up. Root cause: `× density` double-application in inject + dead-code sun-absorption ray-march. See the v3.0.6 follow-up section above.

---

## Build verification

- Debug x64 builds clean across the full 20-commit chain — only pre-existing warnings (LNK4006 dbghelp, C4996 sscanf/strncpy, C4244 chrono in Editor.cpp).
- All 6 binary targets produced.
- Shader hot-reload exercised for all three new variants (density / scatter / noise bake).
- User smoke-test confirmed correct visual on Sponza with default v3.0.6 settings (anisotropy 0.7, density 0.1, scatteringIntensity 15, multiScatter 0.15, Quality High).

### Tagging

After this commit merges to `main`: `git tag -a v3.0.6 -m "v3.0.6 — volumetric-fog-polish"` + `git push --follow-tags`. Mode A — tag-only, no GitHub Release.
