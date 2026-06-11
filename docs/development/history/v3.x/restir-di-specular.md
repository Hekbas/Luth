# material-system.M.6 — restir-di-specular

**Date:** 2026-06-11
**Commits:** on `fix/restir-di-specular` — `c897377` (C++ scaffolding), `be902d9` (shading + combined target), + wrap-up
**Umbrella:** [#151](https://github.com/Hekbas/Luth/issues/151) · **Closes:** [#154](https://github.com/Hekbas/Luth/issues/154)
**Series:** `material-system`, M.6. Mode A — **v3.1.5** PATCH bump, tag-only.

---

## Overview

ReSTIR DI was diffuse-only on **both** layers: `restir_shade.comp` stored `E = Li·NdotL·W` and `pbr.frag`
remodulated `E·albedo·(1−metallic)/π` (zero at metallic=1, roughness never entering) **and** the RIS target
was `Luminance(Li)·NdotL` across initial/temporal/spatial, so the reservoir even *selected* lights for
diffuse. Metals + specular highlights got nothing from point lights under ReSTIR DI; toggling it off (the
Forward+ cluster loop's full Cook-Torrance) showed the break.

M.6 adds a full demodulated specular channel: a combined diffuse+spec RIS target, a demodulated specular
output from the shade pass, a dedicated 4th SVGF denoiser instance, and an F0-remodulation in `pbr.frag`.

## Research (deep-research pass, cited + adversarially verified)

A pre-design research pass (NVIDIA NRD/RTXDI docs, Bitterli 2020, the SIGGRAPH 2023 ReSTIR course, Bevy
0.18 Solari) confirmed:
- **Option A (demodulate + remodulate at composite)** over full-BRDF-in-shade: NRD denoisers operate on
  pure radiance — the BRDF must be decoupled before denoise and re-applied after (3-0). The slim G-buffer
  omits per-pixel F0 anyway, so full-BRDF-in-shade is contraindicated.
- **Separate diffuse/specular denoiser channels** are mandatory (3-0).
- **Canonical RIS target = full BSDF** (Bitterli: ρ·Le·G with ρ the full BRDF). A diffuse-only target is a
  permitted approximation that raises specular variance — so the combined target is the rock-solid choice.
- Production (Bevy Solari, RTXDI) reuses the single light-index reservoir + evaluates specular at shade
  time — no reservoir-structure change needed.

## Design / decisions

- **Combined diffuse+spec RIS target** (`restir_common.glsl::RestirTargetPdf`, used by all three RIS
  passes): `pHat = Luminance(Li)·(NoL + π·D·G/(4·NoV))`. F0-/albedo-free (light selection is
  F0-independent); the π balances the diffuse term's dropped 1/π, so it reduces to the old target when the
  spec lobe → 0. The reservoir now imports the specular lobe into light selection — still unbiased.
- **Demodulated specular output** (`restir_shade.comp`): `S = Li·D·G/(4·NoV)·W`, the F0-free spec lobe
  response (the spec BRDF's `NoL` in `D·F·G/(4·NoL·NoV)·NoL` cancels). Written to a 2nd output image
  (`restirDISpec`, Set 2 b8).
- **Remodulation by F0, NOT envBRDF** (`pbr.frag`): `Lo += diSpecular·F0·restirParams.z`. The key divergence
  from the rt-reflections precedent: reflections store raw VNDF-sampled radiance (lobe handled by importance
  sampling) and apply `envBRDF` (the hemisphere BRDF integral). Direct point-light specular is a SINGLE
  direction with the lobe (D·G) **already applied at shade** — multiplying by `envBRDF` would double-count
  the lobe. F0 (base reflectance) is the correct material factor; the directional Fresnel is approximated by
  F0 (peak), fine for the denoised signal.
- **Dedicated SVGF DiSpecular channel** (4th `SvgfDenoiser` instance) using the **surface-motion** reproject
  (`svgf_reproject.comp`, the diffuse Di pattern) — NOT the rt-reflections hit-distance virtual reprojection.
  Direct point-light specular is surface-attached (the highlight rides the surface), not a reflection's
  virtual image, so it reprojects by surface motion; copying the reflections spec-denoiser would ghost.
- **Gate:** `restirParams.z = (restirActive && specular) ? specularIntensity : 0` (GlobalSubsystem) — one
  vec4 component carries both the gate and the artist intensity.

## Mechanism (C++)

- `RtRestirSubsystem` Set 2 grows 7→9: b7 slimRoughness sampler (combined target + spec shade), b8
  restirDISpec storage image. `AddPasses` returns `{di, spec}`; the shade pass writes both.
- `SvgfDenoiser` is channel-polymorphic — `DenoiserChannel::DiSpecular` + a `Resolve()` arm + the
  `svgfDiSpec*` ViewResources history block (mirrors the GI/Reflections blocks). 4th instance wired in
  `RenderPipeline` + `ViewResources` (alloc / bootstrap-clear / pool sizing / sets / reset).
- `LightingSubsystem` Set 3 grows to b8 (`diSpecular`); `GeometrySubsystem::AddGeometryPass` reads the
  denoised spec handle (barrier-only). `RestirSettings` gains `specular` + `specularIntensity` + RenderPanel UI.

## Denoiser count

End state: **4 SVGF instances** (Di/Gi/Reflections/DiSpecular), runtime-gated per feature. The hard floor
is 2 (diffuse≠specular, NRD-mandated). Consolidating the existing channels (NRD-style dual-channel packing)
is a separate denoiser-architecture effort, deliberately not folded into this fix.

## Scope

C++ + shaders. No new RG mechanism / allocator / sync primitive — composes with the channel-parameterized
`SvgfDenoiser`, the Garlic reservoir tags (unchanged), the ViewResources image pattern, and the
demodulate→remodulate-in-`pbr.frag` seam.

## Verification

- `glslc --target-env=vulkan1.3` clean on the full DI+GI shader surface (14 entry points). A first pass
  validated only the DI shaders + transparents and missed that the shared `restir_common.glsl` is included
  by `restir_gi_common.glsl` — putting `#define GI_PI` + the brdf pull-in there clashed with the GI path's
  `const float GI_PI` (broke `path_trace` + every GI shader at first smoke). Fixed by moving the DI target
  helper (GI_PI + brdf + `RestirTargetPdf`) into its own DI-only `restir_di_target.glsl`.
- Debug x64 MSBuild clean (Luth.lib + Luthien.lib + Luthien.exe), validated in three incremental stages
  (scaffolding, shading, UI).
- Runtime smoke (pending user gate): metal + glossy dielectric under a point light, ReSTIR DI on — specular
  highlight + metal response now appear, match the cluster-loop fallback (no break on toggle) + the PT
  reference; specular denoises cleanly; diffuse DI unchanged-to-slightly-rebalanced (combined target).

## Files

**Scaffolding (c897377):** `RenderPipeline.{h,cpp}`, `ViewResources.cpp`, `subsystems/RtRestirSubsystem.{h,cpp}`,
`subsystems/SvgfDenoiser.{h,cpp}`, `subsystems/GeometrySubsystem.{h,cpp}`, `settings/RestirSettings.h`,
`scene/systems/RenderingSystem.h`.
**Shading (be902d9):** `shaders/common/restir_common.glsl`, `shaders/restir_{initial,temporal,spatial,shade}.comp`,
`shaders/pbr.frag`, `subsystems/LightingSubsystem.cpp`, `subsystems/GlobalSubsystem.cpp`,
`luthien/panels/RenderPanel.cpp`.
