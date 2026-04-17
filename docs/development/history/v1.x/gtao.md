# v1.5.0 — GTAO (Ground Truth Ambient Occlusion)

**Version:** v1.5.0  |  **Date:** 2026-04-17  |  **Epic:** [#58](https://github.com/Hekbas/Luth/issues/58)  |  **Deps:** `compute-gpu-culling` (v1.2.0)

---

## What Was Built

Screen-space ambient occlusion via Ground Truth AO (Jimenez et al. 2016, XeGTAO inspiration). Replaces Luth's flat `ao = 1.0` default ambient term with a physically-grounded occlusion signal that modulates the split-sum IBL contribution, dramatically improving the grounding of objects in scenes dominated by indirect light. Compute-only, mip-0 only (no LDS mip chain), no temporal accumulation yet — an MVP that slots into the existing render graph + compute pass infrastructure shipped in `compute-gpu-culling`.

**Pipeline (per frame, after shadows, before forward shading):**

| Stage | Input | Output | Notes |
|-------|-------|--------|-------|
| **DepthPrepass** (new, opaque-only forward) | Indirect draws | `SceneDepth` (D32) | Enables GTAO to read depth before PBR shades; GeometryPass now loads depth with `LESS_EQUAL` |
| **GTAODepthPrefilter** (compute) | `SceneDepth` | `GTAOLinearDepth` (R32F, half-res) | 2×2 min-gather + perspective linearize; sky pixels clamped to `farZ` |
| **GTAOMain** (compute) | `GTAOLinearDepth` | `GTAORawAO` (R8, half-res) | Horizon-based integral, 2–8 slices, IGN jitter; VS normals reconstructed from depth derivatives |
| **GTAODenoise** (compute) | `GTAORawAO` + `GTAOLinearDepth` | `GTAOFinal` (R8, half-res) | 3×3 tent + bilateral depth weight (~10% relative sigma) |
| **GeometryPass** (`pbr.frag`) | `GTAOFinal` (Set 0 binding 4) | `SceneColor` | `ambient *= gtaoAO` — multiplies material occlusion if present |

- **Z-prepass.** Depth-only forward pass using the camera region of the existing indirect buffer; position-only vertex shader (rigid + skinned variants), reuses `shadowDepth.frag` as null fragment. GeometryPass switched to `LOAD_OP_LOAD` + `VK_COMPARE_OP_LESS_OR_EQUAL` so opaques pass on equal-z. Unblocks both GTAO and the future `forward-plus` (#54) cluster pipeline.
- **GTAOSettings.** Runtime-tunable struct nested in `PostProcessSettings`: `enabled / halfRes / visualize`, `intensity / radius / falloff / power`, `sliceCount (2/3/4/8) / stepsPerSlice`. Editor section in `RenderPanel` with XeGTAO-recommended defaults (radius 0.5 m, falloff 0.615, power 2.0, 3 slices × 2 steps). Mirrored to GPU via a 48-byte std140 `GTAOUBO`, refreshed each frame in `UpdateGTAOUBO()`.
- **Set 0 expansion.** Two new bindings sampled by `pbr.frag`: binding 4 = `sampler2D gtaoTex`, binding 5 = `GTAOUBO`. Descriptor writes live in `UpdateAODescriptors` (called from `InitAOResources` and after `Resize` recreates the half-res textures).
- **Frame Debugger support.** `GTAOLinearDepth / GTAORawAO / GTAOFinal` registered as tracked render targets. Added `R8_Unorm` and `R32_Float` to both `RG::TextureFormat` and `FrameDebugger::ToVkFormat` so archive images allocate at native format instead of silently falling through to RGBA8_UNORM (which previously caused rainbow-banding previews for both GTAO buffers).
- **Visualize mode.** `gtao.visualize` toggles the PBR shader to output the raw GTAO buffer as the scene color — isolates AO contribution for tuning without writing a dedicated debug pass.
- **Always-on chain.** GTAO runs every frame regardless of `enabled`; the shader's `enabled` flag gates the *modulation* inside `pbr.frag`. This avoids first-frame layout-transition ordering issues (the Set 0 binding-4 sampler always sees a valid `SHADER_READ_ONLY_OPTIMAL` layout).

## Bugs Fixed Mid-Epic

- **Frame Debugger preview refresh required cascade-click round-trip.** `m_DepthPreviewKey` (the Phase 14F depth-blit cache key) was never reset when a new capture began — same-archive re-selections after recapture skipped the blit and served stale texture. Matched `m_PerDrawPreviewKey`'s invalidation at `BeginCapture` time.
- **Archive sink format-reinterpretation.** `FrameDebugger::ToVkFormat` is a parallel copy of `RenderGraph::ToVkFormat` and was missing cases for the new GTAO formats, so `vkCmdCopyImage` between the source image and the RGBA8 fallback destination did a raw byte reinterpretation — visible as colored horizontal banding over both `GTAOLinearDepth` (R32_SFLOAT) and `GTAORawAO` (R8_UNORM) previews. Fixed by adding the missing cases to both maps + to `RG::TextureFormat`.

## Files Added / Modified

**New:**
- `luth/assets/shaders/depthPrepass.vert` + `depthPrepass_skinned.vert` — position-only Z-prepass vertex shaders
- `luth/assets/shaders/gtao_depth_prefilter.comp` — half-res min-gather + linearize
- `luth/assets/shaders/gtao_main.comp` — horizon-based AO integral
- `luth/assets/shaders/gtao_denoise.comp` — 3×3 bilateral-depth denoise
- `luth/source/luth/renderer/GTAOSettings.h` — `GTAOSettings` + `GTAOUBO` (std140)
- `luth/source/luth/renderer/passes/DepthPrepass.cpp` — camera-space Z-prepass (`AddDepthPrepass`)
- `luth/source/luth/renderer/passes/AOPass.cpp` — `AddGTAODepthPrefilterPass` / `AddGTAOMainPass` / `AddGTAODenoisePass`
- `docs/development/epics/gtao.md` (deleted at epic close — this file supersedes it)

**Modified:**
- `luth/assets/shaders/pbr.frag` — Set 0 bindings 4/5; GTAO modulation in ambient term; viz early-out
- `luth/source/luth/scene/systems/RenderingSystem.{h,cpp}` — Set 0 layout grows to 6 bindings; `InitAOResources`, `UpdateAODescriptors`, `UpdateGTAOUBO`; GTAO descriptor pool + sampler; frame-graph wiring (`DepthPrepass → GTAO×3 → GeometryPass`); tracked RTs; hot-reload rebuild of all three GTAO pipelines; `m_DepthPreviewKey` invalidation at capture start
- `luth/source/luth/renderer/passes/GeometryPass.cpp` — receive `SceneDepth` handle, `LOAD_OP_LOAD` + LESS_EQUAL
- `luth/source/luth/renderer/{Texture.h,backend/vulkan/VulkanTexture.cpp}` — `R32_Float` format
- `luth/source/luth/renderer/rendergraph/{RenderGraphResources.h,RenderGraph.cpp,RenderResourceCache.cpp}` — `R32_Float` + `R8_Unorm` formats
- `luth/source/luth/renderer/FrameDebugger.cpp` — archive format map gains `R32_Float` + `R8_Unorm`
- `luth/source/luth/renderer/PostProcessSettings.h` — nested `GTAOSettings gtao;`
- `luth/source/luth/editor/panels/RenderPanel.cpp` — "Ambient Occlusion (GTAO)" collapsing section
- `luth/source/luth/core/Version.h` — bumped to v1.5.0

## Out of Scope (Future Polish)

- **XeGTAO parity.** Full 5-mip LDS depth pyramid; edges texture for anisotropic denoising; multi-bounce approximation for diffuse; selective specular attenuation.
- **Temporal accumulation.** Reuses GTAO for free once the `fxaa-taa` epic (#72) lands motion vectors + history buffer.
- **Half-res / full-res toggle.** UI field exists but has no effect yet — always half-res. Trivial to wire once a use case demands it.
- **PostProcessSettings serialization.** GTAO settings reset to XeGTAO defaults per session, matching the existing bloom/tonemap fields. If editor persistence is wanted, extend `EditorSettings` to mirror the fields.
- **AO-aware specular.** Currently multiplies diffuseIBL + specularIBL equally; XeGTAO weights specular with a separate cone-trace term derived from horizons.
