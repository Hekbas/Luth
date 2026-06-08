# rt-renderer.C.2 — svgf-denoiser

**Date:** 2026-06-07
**Commits:** 6 on `feat/svgf-denoiser` (S0 `bf04988`, S1 `dcf6448`, RG primitive `7871e35`, S2 `40b7a84`, arch docs `e1565e8`, wrap-up)
**Issue:** [#147](https://github.com/Hekbas/Luth/issues/147)
**Umbrella:** [#127](https://github.com/Hekbas/Luth/issues/127)
**Series:** `rt-renderer`, Phase C.2. Mode A series-coalesced, **v3.0.13** tag-only, no Release.

---

## Overview

Custom **SVGF** (Schied 2017) denoiser for the noisy ~1-spp demodulated diffuse-irradiance image C.1 ReSTIR DI produces. Inserts between `RestirShade` and `GeometryPass` (all `AsyncCompute`); `pbr.frag` remodulates the *denoised* signal (its `E·albedo·(1-metallic)/π` line untouched). Wrapped in an `IDenoiser` abstraction so a future NVIDIA NRD (`RELAX_DIFFUSE`) swap is a settings toggle. Diffuse-only; A-SVGF (Schied 2018) deferred.

Pass chain: **`svgf_reproject`** (motion reproject + 2×2 bilinear disocclusion + EMA color/luminance-moments) → **`svgf_moments`** (temporal variance, or 7×7 spatial bilateral for `histLen<4` ×`4/histLen`) → **`svgf_atrous ×N`** (5×5 B-spline edge-aware wavelet, squared-weight variance co-filtering, final level writes the consumed image).

This effort leaned hard on **adversarial verification** (the arc's plan calls denoiser tuning the wild card, and the math is unverifiable by eye): a focused deep-research pass nailed the formulas up front, then a shader-math review and a C++ integration review each caught a **critical** bug the author had missed or deferred (below).

---

## Sub-tasks

| # | What landed | Commit |
|---|---|---|
| S0 | **`IDenoiser` interface + scaffolding + pass-through.** `IDenoiser` (NRD-shaped `DenoiseInputs`: di/depth/normal/motion/matID used; roughness/hitDist/confidence reserved) + `SvgfDenoiser` implementing it, hosted by `RenderPipeline::m_Denoise` (`unique_ptr<IDenoiser>`). No-op `svgf_passthrough.comp` copies DI → a new per-view `svgfDenoised` image. The seam goes live: `m_Denoise->AddPasses` between `RestirShade` and `GeometryPass`, the DI handle re-routes, and `LightingSubsystem` Set 3 b5 binds `svgfDenoised` (the denoiser owns the slot whenever ReSTIR is on; disabled = raw passthrough). `SvgfSettings` + RenderPanel section. Verified pixel-identical to C.1. | `bf04988` |
| S1 | **Temporal accumulation.** `svgf_reproject.comp`: 2×2 bilinear motion reprojection, per-tap disocclusion via **relative linear-depth + normal dot** (compute has no `fwidth`, so the proven ReSTIR-temporal form, not Falcor's fragment-shader gradient), EMA color + 1st/2nd luminance moments, per-pixel history-length ramp, `variance = mu2 − mu1²`. Per-view `colorHist`/`moments`/`geom` ping-pong **images** (not Garlic buffers — à-trous needs bilinear taps), GENERAL + bootstrap-cleared. Smoke-confirmed: noise drops on a static camera. | `dcf6448` |
| RG | **GENERAL-preserving compute storage read.** `ResourceState::ComputeReadStorage` + `RenderPassBuilder::ReadStorageImageGeneral` — a storage read that keeps the image in `GENERAL` (plain `ReadStorageImage` transitions to `SHADER_READ_ONLY`, which mismatches a storage `imageLoad` descriptor). Additive (existing `ComputeRead` unchanged), mirroring B.2's pattern of extending the RG with new state arms. The fix for the integration-review bug below; C.3 ReSTIR GI will reuse it. | `7871e35` |
| S2 | **Variance + à-trous.** `svgf_moments.comp` (temporal/7×7-spatial variance) + `svgf_atrous.comp` (5×5 B-spline, depth-gradient/normal/luma edge-stops, variance co-filtered with **squared** weights, 3×3 variance prefilter drives only the luma weight). The reproject→moments→à-trous chain is RG handle-threaded (C.1's import-once-thread-forward pattern); the à-trous final level writes `svgfDenoised`. New `svgfAtrous[2]` within-frame ping-pong. f16 moment-overflow clamp + finite guards (review fix). Smoke-confirmed: penumbra noise resolves, edges preserved, no validation errors. | `40b7a84` |

---

## Design decisions

### feedbackTap = −1 (à-trous is a pure post-filter)
The research flagged feedbackTap as "the single most error-prone SVGF detail" and recommends **feedbackTap = 1** (feed back the 1st à-trous iteration). Shipping −1 instead (the reproject's integrated color is the temporal feedback; the à-trous never touches `colorHist`) is a valid, lower-risk first cut: it halves the descriptor sets (no frame×iter parity), drops the WAR-barrier + conditional-write machinery, and avoids the over-blur trap (the *final*-iteration feedback). feedbackTap = 1 is a deliberate S3/follow-up quality refinement.

### History as VMA images, not Garlic buffers
The plan/`arch/memory.md` assumed SVGF history would use the device-local Garlic buffer path (like C.1's reservoirs). But reading `RecreateViewTextures` showed per-view persistent *images* (`restirDI`, `taaHistory`, volumetric history) are plain VMA `VKTexture`s; only the reservoir SSBOs use the tagged heap. SVGF history needs **bilinear taps** (à-trous + reprojection) → images, following the `restirDI`/`taaHistory` precedent. `arch/memory.md` corrected.

### GENERAL-preserving RG read vs the sampler convention
The integration review recommended the engine's established convention (read cross-pass storage outputs as **samplers** in SHADER_READ_ONLY — GTAO/volumetric do this). But that convention was built for single sampled reads; it's fragile for SVGF's storage ping-pong + cross-frame history: the within-frame sampler read leaves `colorHist`/`moments` in SHADER_READ_ONLY, which then breaks the descriptor-only cross-frame *prev* reads, the frame-0 bootstrap layout, and forces an asymmetry (geom stays GENERAL). The additive `ComputeReadStorage` primitive keeps every history image GENERAL throughout — symmetric, no bootstrap/asymmetry edge cases — and is reusable. The review's rec was deliberately overridden, justified here and in the arch doc.

### Demodulate → denoise → remodulate; three temporal loops
The denoiser operates entirely on the demodulated `E = Li·NdotL·W` (no albedo, no 1/π); `pbr.frag` remodulates the denoised result. Research confirmed this matches NRD's required contract (`IN_DIFF_RADIANCE_HITDIST`) exactly. The three temporal loops stay decoupled: ReSTIR reuse (sample domain), SVGF (radiance domain, this effort), TAA (image domain, downstream, AA-only).

---

## Bugs caught (adversarial verification)

- **f16 moment overflow** (shader-math review, 3-0). `mu2 = luminance²` stored in RGBA16F overflows (>65504) for any DI luminance >256 — easily reached by a bright/close light — producing `+Inf` that poisons variance, collapses the à-trous luma weight (over-blur), and persists via the feedback. Flagged as a risk in S1 reasoning, deferred, then confirmed reachable. Fixed: clamp luminance ≤ 255 before squaring + `isinf/isnan` guards on variance in all three shaders.
- **Moments depth weight** (shader-math review, 3-0, medium). The 7×7 spatial-fallback depth weight used a flat relative tolerance instead of the gradient-projected form the à-trous already uses correctly. Fixed to the central-difference gradient form.
- **Storage-image-read layout mismatch** (integration review, 3-0 across 5 dimensions, **critical**). `builder.ReadStorageImage` transitions to SHADER_READ_ONLY, but the threaded reads bound STORAGE_IMAGE + GENERAL + `imageLoad` → validation error + undefined reads every frame. The author's manual review missed it entirely. Fixed via the `ReadStorageImageGeneral` RG primitive.

---

## Files touched

**Engine — new:** `subsystems/IDenoiser.h`, `subsystems/SvgfDenoiser.{h,cpp}`, `settings/SvgfSettings.h`; shaders `svgf_passthrough.comp`, `svgf_reproject.comp`, `svgf_moments.comp`, `svgf_atrous.comp`, `common/svgf_common.glsl` (+ `.meta`).
**Engine — modified:** `rendergraph/RenderGraph.{h,cpp}` + `RenderGraphResources.h` (the GENERAL-preserving read), `RenderPipeline.{h,cpp}` (m_Denoise + seam + ViewResources fields), `ViewResources.cpp` (history/atrous images + bootstrap clears + pool bumps), `subsystems/LightingSubsystem.cpp` (Set 3 b5 → svgfDenoised), `scene/systems/RenderingSystem.h` (SvgfSettings).
**Editor:** `panels/RenderPanel.cpp` (SVGF section).
**Docs:** `arch/rendering-pipeline.md`, `arch/memory.md`.

---

## Verification

Build clean (Debug x64, luth → luthien → Runtime), no new warnings; all four SVGF shaders pass `glslc --target-env=vulkan1.3`. Two adversarial review workflows (shader-math + integration), each 3-skeptic-verified. Smoke-tested per stage on a point-light penumbra scene: S0 pixel-identical to C.1; S1 noise drops on static camera; S2 penumbra noise resolves with edges preserved, no Vulkan validation errors (the storage-read fix confirmed clean under `LUTH_VALIDATION`).

---

## Hand-off to S3 / C.3 and deviations

**Deferred to a gated follow-up (`asvgf-antilag`, or an S3 quality pass):** **feedbackTap = 1** (feed back the 1st à-trous iteration — research-preferred, needs the frame×iter parity sets + a WAR barrier) and **A-SVGF** (Schied 2018 gradient-driven antilag — substantial machinery: a stratified temporal-gradient buffer + forward-projection + adaptive alpha; build only if penumbra/fast-lighting lag stays objectionable). Both were in the C.2 sub-task sketch but not the locked-decisions table; the research strongly recommended deferring A-SVGF.

**Still open for S3:** bake tuning defaults (σ_l/`phiColor` is the primary knob); surface the reserved `IDenoiser` hit-distance channel for the eventual specular path; optional debug viz (raw/denoised/variance/histLen); optional ReSTIR M-count → confidence prior.

**For C.3 ReSTIR GI:** reuse `ReadStorageImageGeneral` for any storage images threaded across its compute passes, and the `IDenoiser` interface for its diffuse-GI denoise.

**Deviations from the plan/issue:** (1) feedbackTap = −1, not 1. (2) A-SVGF deferred. (3) History = VMA images, not Garlic buffers. (4) Added the `ComputeReadStorage` RG primitive (overriding the integration review's sampler-convention rec). (5) Git hooks not installed in this workspace — comment policy honoured manually.
