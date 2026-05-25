# image-quality

**Date:** 2026-05-25 (v3.0.7)
**Commits:** 6 sub-tasks + wrap-up
**Issue:** [#135](https://github.com/Hekbas/Luth/issues/135)
**Series:** `rt-renderer` Mode A series-coalesced — `Version.h` PATCH bumps `v3.0.6` → `v3.0.7`, tag-only, no Release.

---

## Overview

Closes Phase A of the `rt-renderer` arc. Four image-quality wins, all enabled by infrastructure
landed in earlier A.* efforts:

1. **Specular AA (Tokuyoshi 2019)** — drop-in screen-space normal-curvature variance lifted into BRDF roughness in `pbr.frag`. Kills high-frequency specular sparkle on curved metal at glancing angles. Required migrating `pbr.frag` from its inlined `GlobalUniforms` to `#include "common/globals.glsl"` (v3.0.5 flagged this as future work; opportunistic here).
2. **AgX + AgX Punchy tonemaps** — Wrensch's fitted polynomial from [iolite-engine.com](https://iolite-engine.com/blog_posts/minimal_agx_implementation), Three.js post-r161 lineage (EaryChow-reviewed [PR #27413](https://github.com/mrdoob/three.js/pull/27413)). Punchy preset adds Blender's contrast curve (slope 1.0, power 1.35, sat 1.4) as a second tonemap toggle. ACES remains the default.
3. **TAA (Karis 2014 YCoCg-clip recipe)** — Halton(2,3) prefix-8 jitter on projection + per-view RGBA16F history textures (ping-pong via `frameAbs` parity) + `TaaResolvePass` with the full L2 recipe: closest-depth velocity dilation + 9-tap YCoCg neighborhood + rounded box+plus AABB clamp + chroma narrow + `clip_aabb` toward center + Blackman-Harris 3.3 reconstruction filter + luma-distance feedback + off-screen UV rejection. Reads slim G-buffer motion vectors (RG16F NDC delta from v3.0.1). Runs HDR-domain between volumetric composite and bloom; grid pass writes on top of TAA output.
4. **Blue-noise dither for volumetric slice sampling** (deferred from v3.0.5 polish) — Roberts R2 plastic-number quasi-random sequence baked at engine init (64×64 R8, NEAREST + REPEAT). Sampled in `volumetric_composite.frag` to jitter `sliceW` by ±0.5 slices, breaking up Wronski log-slice Z-banding. TAA integrates the dither over ~6 frames into smooth gradients.

The four pieces are independent in scope but compose: TAA cleans up the dither's per-frame grain, the spec AA fixes a separate aliasing class, and AgX is an artistic A/B swap. Each is individually toggleable in RenderPanel.

Memory: +32 MB per view (two RGBA16F viewport-sized history textures for the ping-pong) × 2 views = +64 MB. Plus 4 KB for the 64×64 R8 blue noise. Negligible against the ~150 MB volumetric atlases per view at High quality.

---

## Sub-tasks

| # | What landed | Commit |
|---|---|---|
| A | **Tokuyoshi19 specular AA.** `Vec4 specAaParams` in `GlobalUniforms` (`x=enabled, y=sigma`) + `common/globals.glsl` mirror. `bool specularAaEnabled = true` + `float specularAaSigma = 0.5f` in `PostProcessSettings`. `pbr.frag` migrated from inlined UBO to `#include "common/globals.glsl"` (drops std140-offset-mismatch trap that blocked adding tail fields), inserts `dFdx(N)/dFdy(N)` variance boost after `clamp(roughness, 0.04, 1.0)`. Default on — zero cost at sigma=0 / flat surfaces. | `e650788` |
| B | **AgX + AgX Punchy tonemaps.** Wrensch polynomial fit (Three.js post-r161 lineage). `TonemapOperator::AgX = 4, AgXPunchy = 5`. `postprocess.frag` gains `agxSigmoid` + `agxLookPunchy` + `AgXBase` helpers. CONTRACT comment: **AgX returns linear sRGB** — the existing tail `pow(color, 1/2.2)` stays untouched (single biggest "ship the wrong thing" trap). RenderPanel `operators[]` extended. | `5f5e091` |
| C | **TAA jitter + history scaffolding.** New `TaaJitter.h` (Halton(2,3) prefix-8 `constexpr` table + `ApplyJitter`). `ViewResources` gains `currentJitter`/`prevJitter` (Vec2) + `taaHistoryA`/`taaHistoryB` (RGBA16F, viewport-sized, persistent) + cycled `taaResolveDescSet[N]`. `PostProcessSubsystem` gains `m_TaaResolveDescSetLayout` (5 bindings — sceneColor, motion, history-prev cycled UAB, sceneDepth, UBO). `GlobalSubsystem::UpdateUBO` applies Halton jitter to projection after Y-flip (gated by `taaEnabled`), caches prev/curr jitter on ViewResources. `Vec4 taaParams` in GlobalUniforms (enabled / temporalAlpha / xy=currentJitter). Per-view pool bumped: maxSets 64→96, samplers 96→128, UBOs 32→48. `RecreateViewTextures` signature gained `fullW, fullH` (TAA history is viewport-sized, not half-res). | `9bdb0c2` |
| D | **TaaResolvePass — full Karis14 L2 recipe.** New `common/taa.glsl` (RGB↔YCoCg + `clip_aabb` + Blackman-Harris 3.3 weights, lifted from [playdead/temporal](https://github.com/playdeadgames/temporal) MIT, attributed in shader header). New `taa_resolve.frag` (~170 LOC): 9-tap 3×3 in YCoCg → BH-reconstructed center → rounded box+plus AABB → chroma narrow (¼ luma extent) → closest-depth velocity dilation → off-screen UV rejection → `clip_aabb` toward AABB center → luma-distance feedback weight → blend → YCoCg→RGB. Push constant carries `temporalAlpha`. `AddTaaResolvePass` + `WriteTaaResolveView` (stable bindings 0/1/3) + `WriteTaaResolvePerFrame` (binding 2 = parity-picked history-prev). Inserted in `RenderPipeline::Execute` between volumetric composite and bloom; grid pass now writes on TAA output. | `b7b631d` |
| E | **Blue-noise volumetric slice dither.** New `blue_noise_bake.comp` — Roberts R2 plastic-number quasi-random sequence (one-line math, no iteration, blue-noise-LIKE spectrum without full void-and-cluster cost). `m_BlueNoise2D` + `m_BlueNoiseSampler` (NEAREST + REPEAT) on `VolumetricSubsystem`, baked once at `Init` via `ImmediateSubmit` mirroring the existing 3D Worley block byte-for-byte. Composite descriptor set layout gains binding 2 (stable per-view), written in `WriteCompositeView`. `volumetric_composite.frag` jitters `sliceW` by ±0.5 slices when `ubo.volScatterParams.y != 0`. `VolumetricSettings::blueNoiseDither = true` toggle plumbed via `GlobalSubsystem::UpdateUBO`. | `04905e9` |
| F | **RenderPanel UI + wrap-up v3.0.7.** New "Anti-Aliasing" collapsing header between Volumetric Fog and Bloom — TAA enable + temporalAlpha slider + Specular AA enable + sigma slider + tooltips. "Blue-Noise Dither" toggle added to Volumetric Fog → Temporal section. `Version.h` 3.0.6 → 3.0.7. This history file. ROADMAP A.5 row → done. `arch/rendering-pipeline.md` pass-order updated. | this commit |
| G | **Fix: TAA static-camera jitter.** Resolve-side de-jitter via push constant. Smoke gate after F showed Karis14 TAA failing to converge on a fully static scene — output essentially equalled the jittered current render every frame. Root cause: slim G-buffer motion vectors carry the Halton jitter delta (~1 pixel for static), bilinear sampling of `historyPrev` at that sub-pixel offset never converges on high-frequency content. Fix: extend `taa_resolve.frag`'s push constant to `{float alpha, float pad, vec2 jitterDeltaUv}`, populate `jitterDeltaUv = (currentJitter − prevJitter)/viewport` from existing `ViewResources` state, add it to the per-pixel motion so static scenes net to zero motion. Production engines (UE4 / HDRP / playdead) de-jitter at the producer; doing it in the resolve keeps slim_gbuffer untouched. | post-wrap fix |

---

## Architectural decisions

### Why K3 (YCoCg AABB clip) over K4 (Salvi variance-clip)

The plan-mode research surveyed four neighborhood-rejection variants:
- K1: Karis14 RGB AABB clamp
- K2: Salvi16 RGB variance clip
- K3: Karis/Pedersen YCoCg rounded AABB clip
- K4: YCoCg variance clip (UE4-style)

Modern production engines (UE4 base TAA, Frostbite, CoD, Decima per [The Code Corsair survey](https://www.elopezr.com/temporal-aa-and-the-quest-for-the-holy-trail/)) ship K4 or K3 — both clearly outperform RGB variants on the "purple fringe" hue-shift artifact. The K4-over-K3 delta is meaningful only in scenes with high-contrast pinprick outliers (specular foliage, far-LOD pinpoints). The engine doesn't have those scenes yet.

K3 is what playdead INSIDE shipped (with the `USE_YCOCG` keyword enabled). Its `clip_aabb` and `RGB_YCoCg` helpers lift line-for-line from their MIT-licensed reference shader — known-good baseline, cite Karis14 + Pedersen16 + playdead in the shader header. K4's μ/σ math is sensitive to the γ tunable and to neighborhood window size — Salvi himself flags variance-induced flicker as the failure mode on slide 38 of his GDC 2016 deck.

The cost of K3→K4 later is ~25 LOC and zero architectural commitment (μ/σ replaces min/max; `clip_aabb` unchanged). The reverse — downgrading K4 to K3 because of variance flicker — is more annoying. Ship the known-good baseline.

### The supporting cast matters more than the clamp choice

Karis 2014 makes a strong claim worth restating: the clamp variant is responsible for maybe 5% of TAA's perceived quality. The real wins are:

1. **Closest-depth velocity dilation** (~12 LOC) — sample motion at the closest-depth neighbor in 3×3 so silhouette edges follow the OCCLUDER's motion. Fixes the disocclusion ghosting class that no clamp variant can address.
2. **Blackman-Harris 3.3 reconstruction filter** (~15 LOC) — Gaussian-fit weights (center 0.5, edge 0.1145, corner 0.0094). The center-heavy 3×3 reconstruction IS the "current" pixel that gets blended with history. Without it TAA blurs sub-pixel features; this is NOT a post-sharpen.
3. **Luma-distance feedback weight** (~6 LOC) — Karis's anti-flicker mechanism. Lerps between heavy history use when luma matches and lighter history when it diverges. Squared curve for soft falloff.
4. **YCoCg color space + chroma narrow** (~16 LOC + 4 LOC) — separately constrains chroma extent to ¼ of luma extent. Fixes "purple fringe" RGB AABB exhibits.
5. **Off-screen UV rejection** (~2 LOC) — frame 0 settles via this path (prev VP is identity → motion vectors huge → reproject outside [0,1]).

All five ship in the same `taa_resolve.frag` commit (D). Skipping any one (especially 1 or 2) leaves visible quality on the table. The shader header explicitly lists which references each chunk implements.

### Halton(2,3) prefix-8, not N=16

Per the plan-mode research: UE4/UE5 base TAA defaults to N=8. Karis 2014 used N=8. The ONE shipping reference for N=16 is INSIDE — but Pedersen explicitly notes they paired N=16 with a looser 3×3 RGB min/max clip ("after moving to 3×3 and clipping, switched to 16 indices"). That's the OPPOSITE pairing from K3.

With tight YCoCg variance clip + chroma narrow, longer sequences hurt convergence (the clamp aggressively rejects mid-cycle history samples). Karis's N=8 + K3 is the canonical pairing the rest of the industry adopted.

At 60 FPS, N=8 = 133 ms full cycle; N=16 = 267 ms. INSIDE could afford that latency; action-rate engines generally can't. The escape hatch: change `kHaltonJitter` to a 16-entry table if a profiling pass justifies it later — a one-line change.

### HDR-domain TAA (pre-tonemap)

The target pass-order diagram in `arch/rendering-pipeline.md` puts TAA AFTER volumetric composite and BEFORE bloom — i.e. HDR-domain. Karis recipe assumes HDR input; bloom blooms the AA'd HDR signal (otherwise bloom haloes around aliased edges). Tonemap then converts the resolved HDR to LDR.

Confirmed in code: `taaHistoryA/B` are RGBA16F. Resolve writes to `taaCurr` (parity-picked of A/B). Bloom reads `taaCurr`. Grid pass also writes to `taaCurr` (in-place compose). Composite reads `taaCurr` + bloomFinal → LDR.

Grid pass writes in-place on the TAA output. This is important: without it, the grid would be drawn on un-TAA'd color (the old `fogColor`) and then bloom would consume `fogColor` while composite consumed grid+fogColor — order would diverge. Routing grid through TAA's output keeps the chain coherent.

### Per-view jitter state on ViewResources

`GlobalSubsystem` is shared across views (Scene + Game panel) — a single `m_CurrentJitter` would cross-contaminate. Per-view storage on `ViewResources` (next to the existing `prevViewProjection`) avoids the multi-view contamination hazard documented in v3.0.1 `slim-gbuffer.md` and `arch/rendering-pipeline.md` hazard #2.

`GlobalSubsystem::UpdateUBO` reads/writes the per-view jitter state via `m_Pipeline->GetCurrentViewResources()` — same hook as the existing `prevViewProj` caching.

### AgX returns linear sRGB (the trap)

Plan-mode research surfaced this as "the single biggest 'you'll ship the wrong thing' trap." The Wrensch AgX function ends with `color = pow(max(color, 0), vec3(2.2))`. That exponent is **2.2, not 1/2.2** — a sRGB EOTF *decode*, not display encode. The sigmoid output sits in sRGB-OETF-shaped space; the `pow(2.2)` re-linearizes it. AgX returns **linear sRGB**.

The engine's `postprocess.frag` tail does `color = pow(max(color, 0), vec3(1.0/2.2))` for display encode — that line MUST stay. The CONTRACT comment above the AgX function in the shader is loud:

> `// CONTRACT: AgX returns LINEAR sRGB. The pow(color, 1/2.2) tail at end of main() does the display`
> `// encode. The pow(color, vec3(2.2)) inside AgX is a *decode* — the sigmoid output sits in sRGB-OETF`
> `// space and this re-linearizes it. DO NOT remove the tail gamma (washed-out image) and DO NOT flip`
> `// the inner pow to 1/2.2 (too-dark image).`

If a future contributor "simplifies" by removing the tail gamma or flipping the inner pow, this comment is the load-bearing context.

### pbr.frag migrated to common/globals.glsl

Sub-task A needed to add `Vec4 specAaParams` at the tail of `GlobalUniforms`. The existing `pbr.frag` had its own inlined `GlobalUniforms` struct stopping at `farZ`, missing the volumetric tail fields. Appending `specAaParams` after `farZ` in pbr.frag alone would compute a DIFFERENT std140 offset than the C++ struct (where it lands after all the vol* fields).

Three resolutions:
- **A (chosen)**: Migrate pbr.frag to `#include "common/globals.glsl"` — clean. The v3.0.5 history flagged this as future work; opportunistic now.
- B: Duplicate all intervening vol* fields into pbr.frag's inlined struct — preserves the duplication v3.0.5 was supposed to eliminate.
- C: Use a push constant for spec AA — adds pipeline-layout work for a single bool + float.

The migration drops some stale inline comments from pbr.frag (`// Per-cascade light-space matrices (Phase 13)`, etc.) that violated the comment policy anyway. Net win.

### Roberts R2 over true void-and-cluster

The plan called for a "void-and-cluster" blue noise bake. Implementing void-and-cluster in a single compute shader is non-trivial — iterative refinement is the canonical approach, ~150 LOC of compute shader.

Roberts' R2 plastic-number quasi-random sequence ([Martin Roberts, 2018](https://extremelearning.com.au/unreasonable-effectiveness-of-quasirandom-sequences/)) is **one line of math** with a blue-noise-LIKE spectrum:

```glsl
const float g = 1.32471795724474602596;  // plastic number ρ
const vec2  a = vec2(1.0/g, 1.0/(g*g));
float value   = fract(0.5 + dot(vec2(p), a));
```

The irrational coefficients ensure no 2D periodicity within the tile; `fract()` wraps cleanly so REPEAT sampling is seamless. After TAA integrates the dither over ~6 frames, Roberts R2 is indistinguishable from true blue noise — both produce uniform smooth gradients. Before TAA integrates (single-frame view), Roberts R2 is slightly more periodic than true blue noise but still high-frequency enough to break up banding.

The bake shader is ~30 LOC. The C++ side mirrors the existing 3D Worley bake byte-for-byte (~90 LOC). Net: ~120 LOC vs ~250 LOC for true void-and-cluster, with visually indistinguishable results once TAA is in.

If a future scene exposes Roberts R2's slightly-periodic per-frame appearance as a problem (unlikely without TAA off), swap to a precomputed void-and-cluster pattern as a baked PNG asset — sample at the same binding without touching the consumer.

---

## Known issues / follow-ups

### Salvi K4 variance-clip upgrade

The plan-mode research recommended K3 over K4 for the first ship because the engine doesn't currently have scenes that exercise K4's quality advantage (high-contrast specular pinpricks on foliage / far LODs). When such a scene appears (Phase D RT reflections will likely introduce them), upgrade K3 → K4 in `taa_resolve.frag` by replacing the box+plus min/max with first-two-moment variance bounds (μ ± γ·σ, γ=1). ~25 LOC delta; `clip_aabb` unchanged.

### Mitchell-Netravali post-sharpen pass

Karis 2014 explicitly notes TAA's natural blur can be compensated by a Mitchell-Netravali post-sharpen pass after resolve. UE4 ships `r.Tonemapper.Sharpen`; CoD ships CAS. The Blackman-Harris reconstruction in the resolve gets most of the win — but a separate sharpen would close the remaining gap. Deferred — it's its own pass with bloom/exposure interactions, and the "everyone wants a sharpness slider" UX work is non-trivial.

### Motion-blur fallback for high velocity

Pedersen INSIDE shipped a `k_trust = invlerp(15, 2, ‖v‖)` weight that fades history to motion-blurred current at velocities > 15 pixels. Without per-object motion blur this currently routes to the off-screen UV rejection path (high motion → reproject outside [0,1] → fall back to current). Sufficient until per-object motion blur lands as its own effort.

### ShadeMode::TaaHistory debug viz

The slim viz pipeline (sub-task A.2 SlimVizPass) could be extended with a TaaHistory mode that blits `taaHistoryCurr` over LDR for direct inspection. Deferred — the frame debugger already shows pass outputs, so the live ShadeMode toggle is convenience, not necessity.

### Roberts R2 vs precomputed blue noise

If smoke shows Roberts R2's slight per-frame periodicity as visible grain (with TAA off in some debug viz), swap to a precomputed void-and-cluster pattern as a baked PNG asset. Sampling code unchanged; replace the `blue_noise_bake.comp` dispatch with a PNG upload. Defer until the scene that triggers it appears.

### Blackman-Harris weight normalization

The 3×3 BH weights in `common/taa.glsl` sum to 0.9956 (canonical Pedersen sum ≈ 1.0004). Slight under-weighting causes per-frame dimming of ~0.044% at default feedback — imperceptible but a cleanup candidate. Not the jitter cause (caught while reviewing the resolve in sub-task G).

### Source-side de-jitter for RT denoising

The sub-task G fix de-jitters at the resolve. The cleaner long-term shape — used by UE4 / HDRP / playdead — computes motion vectors with UNJITTERED prev/curr VPs at the slim G-buffer stage, so motion vectors carry pure rigid displacement reusable by other consumers (motion blur, RT denoising). Cost: two new Mat4 fields in `GlobalUniforms` (`viewProjectionUnjittered`, `prevViewProjectionUnjittered`) + a `ViewResources::prevViewProjUnjittered` field + updates in `slim_gbuffer.vert`. Defer to Phase B `rt-extensions` / `blas-tlas` when RT denoising needs the cleaner motion convention.

### AgX exposure-aware curve fit

Wrensch's polynomial is a 16.5-stop fit centered at middle gray. If users push extreme exposures (`pp.exposure > 5` or `< 0.2`), the fit deviates from Blender's reference LUT. The CONTRACT comment notes this; the workaround is to stay within ±2 stops of unity exposure. A future enhancement could swap to Filament's expanded-range fit at a 10-20 LOC cost.

---

## Bugs caught during implementation

- **TAA static-camera jitter** — fixed post-wrap (sub-task G). Smoke gate after the wrap commit showed TAA failing to converge on a fully static scene because slim G-buffer motion vectors include the Halton jitter delta; bilinear history sampling at the sub-pixel offset can't converge. Resolved by adding `(currentJitter − prevJitter)/viewport` to the resolve's motion vector via push constant. Source-side de-jitter (slim_gbuffer + unjittered prev/curr VPs on the UBO) is the long-term form; defer to Phase B when RT denoising wants pure rigid-displacement motion vectors.
- The pbr.frag std140-offset gotcha was caught during planning, not implementation (the v3.0.5 deferred-migration note flagged it). Build was clean across all seven commits with only pre-existing warnings (LNK4006 dbghelp / vulkan-1, C4244 chrono in Editor.cpp, C4996 sscanf in Properties.cpp).

---

## Build verification

- Debug x64 builds clean across the full 6-commit chain — only pre-existing warnings as listed above.
- All 5 binary targets produced (Luth.lib, Luthien.lib, Luthien.exe, JobSysProof.exe, LuthTests.exe).
- Shader compile clean for all 5 new/modified GLSL files (pbr.frag migration, postprocess.frag AgX, taa_resolve.frag, common/taa.glsl, blue_noise_bake.comp, volumetric_composite.frag dither).
- User smoke gate: pending before `--no-ff` merge to main.

### Tagging

After this commit merges to `main`: `git tag -a v3.0.7 -m "v3.0.7 — image-quality"` + `git push --follow-tags`. Mode A — tag-only, no GitHub Release. Phase A.5 closes Phase A; the milestone Release will be `rt-renderer-arc-close` at series end.
