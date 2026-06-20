# engine-optimization (v3.5.0)

**Date:** 2026-06-20 · **Issue:** [#164](https://github.com/Hekbas/Luth/issues/164)

GPU-side perf pass over the RT renderer. A v3.4.0 audit plus a Tracy capture put the frame as GPU work-bound on the async-compute RT chain (ReSTIR DI/GI, SVGF, reflections, volumetric), graphics queue near idle. The levers below cut that chain's work or stop it running when it shouldn't. All half-res work is behind default-off Render-panel toggles. #164 stays open for the remaining levers (the two RT traces, trim-SVGF, DRS).

## Measure
Bottleneck-triage plots (acquire/present, PSO compile, GPU-object-cap + IOThread-drop counters). Verdict at 1400x800 on a 3080: ~6-7ms compute + ~1ms graphics ≈ 7.5ms (~135fps); 60fps target so under budget. Per-pass: GiInitial 1.43ms + RtReflections 1.05ms (~39% of compute), SVGF across 4 channels ~2ms (~31%).

## Half-res ReSTIR DI / GI / Reflections
Trace + denoise each channel at half resolution, then a joint-bilateral (depth/normal) upscale back to full. One shared `bilateral_upscale.comp`. The SVGF chain became scale-aware: it detects half-res from the channel's history-texture extent vs the full denoised image (no per-pass setting plumbing) and remaps G-buffer reads half→full via a `gbufferScale` + `dispatchW/H` push constant. Trace shaders gained the matching `GbufCoord` / `SvgfGuide` remap. Each channel sits behind a `halfResolution` toggle on its settings struct; DiSpecular rides the DI toggle. GI measured ~17% (10.6→8.8ms); reflections was the last full-res denoiser chain.

## Gating (work that ran when it shouldn't)
- **DiSpec SVGF** gated on `RestirSettings::specular`. pbr already zeroes its composite when specular is off, so the whole denoise chain ran dead (~0.5ms).
- **PathTrace** was running the *entire* realtime pipeline on top of the path tracer. Root cause: `CullDeadPasses` keeps any pass alive if it has color/depth attachments **or writes an external (imported) resource**, and every ReSTIR/SVGF/GI/reflections/volumetric pass imports + writes its persistent reservoirs/history/atlases, so dead-cull can never drop them. Fix: `BuildGraph` skips *registering* the realtime passes when PT is active. The `ptEnabled` flag folds in a TLAS-ready check so it implies `ptActive` (a cold boot renders one realtime frame to warm the TLAS, no black frame). Denoisers/upscales then auto-skip via their invalid-input early-returns. Kept in PT: Deform, cluster + light-assign (keep the lighting set valid for PT's NEE light read), TLAS, PathTrace, post.
- **GTAO + bloom** skip when off. No bootstrap needed: the VKTexture ctor leaves un-written color targets in `SHADER_READ_ONLY`, so the consumer binding stays valid and ignores the stale content via its flag (`gtao.enabled`, `bloomStrength`). Bloom add guarded in `postprocess.frag` to stay NaN-safe against an uninitialized RGBA16F bloom texture.

## Files
`renderer/RenderPipeline.{cpp,h}` (orchestration + ViewResources half buffers + the PT gate), `renderer/ViewResources.cpp` (half alloc + descriptor-pool sizes), `subsystems/{RtRestirSubsystem,RtRestirGiSubsystem,ReflectionsSubsystem,SvgfDenoiser}.cpp`, the settings structs, shaders (`bilateral_upscale.comp`, `svgf_spec_reproject.comp`, `rt_reflections.slang`, `postprocess.frag`), `RenderPanel.cpp` toggles.

## Verify
A/B each toggle in the Render panel. User-validated: half-res GI/DI/Reflections look right, PT capture collapses to ClusterBuild/LightAssign/TlasBuild/PathTrace on the compute lane, GTAO + bloom skip cleanly when off. Builds green Debug x64 throughout, no new validation errors.
