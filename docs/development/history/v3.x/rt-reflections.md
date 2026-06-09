# rt-renderer.D.1 — rt-reflections

**Date:** 2026-06-09
**Commits:** on `feat/rt-reflections` (S0 `cd933ad`, S1 `0cd3c0b`, S2 `13ec7fb`, S2-fix `b58104b`, S3 `d88b273`, S4 `edd2b76`, wrap-up)
**Issue:** [#149](https://github.com/Hekbas/Luth/issues/149)
**Umbrella:** [#127](https://github.com/Hekbas/Luth/issues/127)
**Series:** `rt-renderer`, Phase D opener. Mode A series-coalesced, **v3.0.17** tag-only, no Release.

---

## Overview

Stochastic **ray-traced specular reflections** + a dedicated **specular denoiser** — the SSR-superseding
reflection path. One GGX-VNDF-importance-sampled reflection ray per opaque pixel is cast from the slim
G-buffer (oct normal + roughness + depth), shaded at the hit via the bindless geometry table (emission +
point/sun NEE + diffuse IBL ambient) or the sharp environment on miss, denoised by a third `SvgfDenoiser`
instance, and composited into `pbr.frag`'s split-sum specular IBL below a roughness cutoff. Hero: Bhaal
Temple's damp floor + altar metal.

The reflection lobe IS the GGX specular — `path_trace.comp`'s GGX-VNDF + Cook-Torrance was extracted to a
shared `common/brdf.glsl` (SPIR-V-verified equivalent) and reused. The denoiser is the effort's wild card:
it reuses the C.2 SVGF reproject/moments/à-trous machinery via the channel-parameterized `SvgfDenoiser`,
but its reproject is a **specular variant** doing **hit-distance virtual reprojection** (a reflection's
screen motion differs from the surface's, so the diffuse motion-vector reproject ghosts mirrors).

Two adversarial-review workflows (S2 trace + Contract math; S3/S4 denoiser + composite) ran against the
real committed code.

---

## Sub-tasks

| # | What landed | Commit |
|---|---|---|
| S0 | **Subsystem seam + per-view image.** `ReflectionsSubsystem` (clone of `PathTraceSubsystem`): Set 2 (b0 output + b1-b3 slim inputs), stub test-pattern shader, inline AS barrier (dst=COMPUTE), `needTlas` gate, AsyncCompute pass kept alive via `SetHasSideEffect`. Per-view `reflRadiance` RGBA16F + `ReflectionsSettings` + `"Reflections"` NamedTexture. | `cd933ad` |
| S1 | **Extract `common/brdf.glsl`.** GGX D/G/F + Cook-Torrance + GGX-VNDF + lobe pdfs lifted verbatim from `path_trace.comp`; `#include`d back. SPIR-V verified functionally identical (same 1342-instruction multiset; only ID numbering + emission order differ — byte-identical is impossible with a shared include straddling the interleaved `DirectLighting`). | `0cd3c0b` |
| S2 | **GGX-VNDF reflection trace.** Reconstruct worldPos/normal/roughness from the slim G-buffer, roughness-cutoff skip, `SampleGGXVNDF` → `reflect(-V,H)`, opaque rayQuery, `FetchHitSurface` hit shading (emission + point/sun NEE + diffuse IBL ambient), sharp env on miss. Output = demodulated lobe-mean radiance (rgb) + world-space hitDist (a). `ReflPC` push constants. | `13ec7fb` |
| S2-fix | **Review hardening.** Grazing-robust ray origin (offset grows as R grazes — shading-vs-geometric normal divergence → glossy self-hit); softened the contract wording to honest "consistent-in-the-mean". | `b58104b` |
| S3 | **Specular denoiser channel.** `SvgfDenoiser` `DenoiserChannel::Reflections` (3rd instance): `Resolve`/`Settings`/`PassName` + `svgfSpec*` history. New `svgf_spec_reproject.comp` (hit-distance virtual reprojection + hitDist-gated disocclusion + roughness tuning); moments/à-trous reused unchanged. `ubo.invViewProjection` added. `denoisedReflHandle` threads through GeometryPass (keep-alive). `SvgfSpecSettings`. | `d88b273` |
| S4 | **Composite + editor.** `pbr.frag` Set 3 b7: `mix(prefiltered, denoised, reflWeight)·Fenv` (roughness-faded, `reflParams.x`-gated). `LightingSubsystem` b7 binds `svgfSpecDenoised`. `reflParams` UBO (GlobalSubsystem fill). RenderPanel "RT Reflections" + "SVGF (Specular)" sections. | `edd2b76` |

---

## Design decisions

### Contract X — split-sum-consistent demodulation (no primary F0)
The slim G-buffer carries no albedo/metalness, so the trace cannot compute the primary surface's F0. The
trace therefore outputs the **VNDF-sampled incoming radiance Li** (an estimate of the GGX-lobe-mean
radiance — the same quantity `prefilteredColor` approximates), and `pbr.frag` applies the split-sum
env-BRDF `(F·brdf.x + brdf.y)` once. `E_VNDF[F·G1(l)] = Fenv` by the LUT's construction, so the round-trip
equals the true specular under the split-sum's radiance/BRDF decorrelation — the SAME approximation the
existing IBL already makes, and accurate exactly where RT is used (low roughness; the cutoff fades to IBL
before the decorrelation loses accuracy). The adversarial review confirmed the exact `Li·F·G1` form
(Contract Y) is more correct but needs the F0 the G-buffer omits; X is the pragmatic, IBL-consistent choice.

### Env-on-miss at LOD 0 (sharp)
The VNDF sample already spread `R` by roughness in ray space; a roughness-LOD env prefilter would
double-apply the spread. The per-sample sharp env is the correct 1-spp radiance; the denoiser converges it.

### Specular denoiser = SvgfDenoiser channel + a specular reproject (not a thin spec-constant)
The plan-time review (finding 3) showed specular needs hitDist coupled to BOTH reprojection AND
accumulation, which a specialization-constant on the diffuse shaders can't retrofit. Resolved to a
genuinely specular reproject (`svgf_spec_reproject.comp`): hit-distance virtual reprojection (reconstruct
the receiver, mirror-reflect the view to `worldPos + reflect(-V,N)·hitDist`, reproject via
`prevViewProjection`) + hitDist carried in the geom-history `.a` for reflected-depth disocclusion + a
roughness-loosened hitDist gate. moments/à-trous reused unchanged (their primary-surface depth/normal
edge-stops are correct for specular too). No descriptor-layout divergence: the spec reproject reuses the
diffuse reproject layout, reinterpreting b3 as roughness (WriteView branches) and computing the
reflection's motion internally; `ubo.invViewProjection` unlocked the virtual reprojection without
push-constant branching.

### NOT ReSTIR'd, 1-spp, opaque-only
The narrow GGX lobe makes spatial reservoir reuse unhelpful + glossy reconnection hard — industry shape is
1-spp GGX + a specular denoiser (NRD ReBLUR). No reservoir buffers. Opaque-only (`gl_RayFlagsOpaqueEXT`),
consistent with the rest of the RT path.

---

## Bugs caught (adversarial verification)

- **Demodulation contract wording + the grazing ray origin** (S2 review). The contract's "unbiased" claim
  was an over-statement (it's consistent-in-the-mean under decorrelation); the env-miss LOD0 was confirmed
  correct (not under-blurred — the denoiser converges it). The ray-origin offset now grows as `R` grazes
  (the slim shading normal can diverge from the geometric surface → glossy-grazing self-hit). False
  positives correctly rejected: the QUEUE_FAMILY_IGNORED "bug" (CONCURRENT sharing mandates it), the "pbr
  composite double-counts" (that stage was unbuilt at S2), and the point-light `dist` clamp (a shared
  helper; the post-shade firefly clamp suffices).
- **S3/S4 review — clean (no code fixes).** All findings were false positives or precedent-matching: the
  "uninitialized b7 → garbage" composite claim is foreclosed by the `reflParams.x = enabled && TLAS` gate
  (svgfSpecDenoised is written-before-read each frame, like the shipped svgfDenoised/svgfGiDenoised); the
  b7 layout-mismatch-when-disabled is the identical correlated-dead-access pattern as b5/b6 (no core VUID);
  the `DenoiseInputs.motion`-slot-carries-roughness "naming wart" is intentional (it's the spec reproject's
  explicit b3 RG dependency — the suggested cleanup would make it implicit/fragile). The one real residual
  — sub-pixel jitter wobble in the virtual reprojection (the matrices carry mismatched frame-N/N-1 Halton
  jitter) — is bounded + absorbed by the bilinear+EMA; deferred (fix = un-jittered VP) unless the smoke
  test shows mirror shimmer under camera motion.

---

## Files touched

**Engine — new:** `subsystems/ReflectionsSubsystem.{h,cpp}`, `settings/ReflectionsSettings.h`; shaders
`rt_reflections.comp`, `common/brdf.glsl`, `svgf_spec_reproject.comp` (+ `.meta`).
**Engine — modified:** `RenderPipeline.{h,cpp}` (m_Reflections + m_DenoiseRefl + BuildGraph seam + needTlas
+ ViewResources fields), `ViewResources.cpp` (reflRadiance + svgfSpec* history + bootstrap + pool),
`subsystems/SvgfDenoiser.{h,cpp}` (DenoiserChannel::Reflections + spec reproject), `subsystems/{Global,
Lighting,Geometry}Subsystem.{h,cpp}` (invViewProjection + reflParams + Set 3 b7 + AddGeometryPass param),
`scene/systems/RenderingSystem.h` (ReflectionsSettings + SvgfSpecSettings + invViewProjection + reflParams).
Shaders `path_trace.comp` (#include brdf.glsl), `pbr.frag` (Set 3 b7 composite), `common/globals.glsl`
(invViewProjection + reflParams).
**Editor:** `panels/RenderPanel.cpp` ("RT Reflections" + "SVGF (Specular)" sections).

---

## Verification

Build clean (Debug x64, luth → luthien → Runtime), no new warnings; all `.comp`/`.frag` pass
`glslc --target-env=vulkan1.3`. S1 SPIR-V equivalence proven (ID-normalized instruction streams identical).
Two adversarial review workflows (S2 trace/contract, S3/S4 denoiser/composite). Runtime smoke-test pending
before merge.

---

## Hand-off / deviations

- **Deferred refinements (the escalation hatch):** a full ReBLUR-class specular denoiser (NRD-style
  coupled hitDist accumulation + anti-lag + parallax-corrected history) if glossy ghosting stays
  objectionable; roughness-scaled à-trous blur radius; the exact `Li·F·G1` contract (Contract Y) if the
  slim G-buffer ever carries F0; un-jittered VP for the virtual reprojection (the TAA jitter wobbles it
  sub-pixel today).
- **Opaque-only** RT path stays consistent (cutout/transparent treated opaque).
- Git hooks not installed in this workspace — comment policy honoured manually.
