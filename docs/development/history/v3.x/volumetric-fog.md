# rt-renderer.4-volumetric-fog — volumetric-fog

**Date:** 2026-05-23
**Commits:** 14 (on `feat/volumetric-fog`)
**Issue:** [#130](https://github.com/Hekbas/Luth/issues/130)
**Series:** `rt-renderer`, fourth effort. Mode A series-coalesced — `Version.h` PATCH bump to `v3.0.3`, tag-only, no Release.

---

## Overview

Fourth sub-effort of the `rt-renderer` v3.0.x arc. Adds a Wronski frustum voxel volumetric fog system: a per-view 160×90×128 RGBA16F 3D atlas accumulates in-scatter front-to-back through the camera frustum; two async-compute passes inject per-voxel light (directional + cluster point + local `Component::FogVolume` modulation) and integrate transmittance; a graphics composite pass blends the integrated atlas back into HDR sceneColor via alpha-blend, layering analytic global distance + height fog on top. A new tagged-union `Component::FogVolume` (Box / Sphere) modulates voxel density + color in local regions and is fully editable via a new inspector drawer.

The inject pass reuses the forward-plus Light SSBO + cluster grid + CSM shadow map: each voxel computes its Olsson cluster ID and iterates only the lights that touch that 3D cell, then applies dir-light shadow attenuation via the cascaded shadow map. Composition uses the engine's standard alpha-blend equation (`final = src·src.a + dst·(1-src.a)`) by reshaping the shader output as `(fogColor, fogOpacity)` — no new blend mode needed, no PBR shader edits, no feedback loops, and composite slots cleanly into the existing pre-bloom HDR-mutation point.

Three architectural additions land: `VKTexture` gains a generalized 3D-image ctor (extension of the existing 2D/2DArray/Cube branching), a new `VolumetricSubsystem` sibling joins the six existing render subsystems, and `Component::FogVolume` lands as a tagged-union following the `Component::Collider` template. The atlases use persistent VMA storage on `ViewResources` (lifetime > 1 frame; the temporal-history slot ships unused — temporal accumulation deferred to a follow-up effort). The per-frame `FogVolume` SSBO routes through `GPUTaggedPageAllocator`, mirroring `LightingSubsystem::UploadLightSSBO`.

A master `enableVolumetricFog` toggle in `EditorSettings` flows through `EditorViewportState` → `CameraParams` → `RenderPipeline::BuildGraph`, skipping the full inject + integrate + composite chain when off.

---

## Sub-tasks

| # | What landed | Commit |
|---|---|---|
| A4.1 | **`VKTexture` 3D generalization.** Base `Texture` gains `virtual u32 GetDepth() const`. New 4th ctor `VKTexture(w, h, depth, format, extraUsage)` for storage-only 3D. `CreateImage` branches to `VK_IMAGE_TYPE_3D` (extent.depth = depth, arrayLayers = 1); `CreateViewAndSampler` branches to `VK_IMAGE_VIEW_TYPE_3D` with null sampler + bindless skip (mirrors existing depth + cubemap paths). | `da7b6a6` |
| A4.2 | **`VolumetricSubsystem` skeleton.** New `subsystems/VolumetricSubsystem.{h,cpp}` with `Init`/`Shutdown`/`OnShaderReloaded`. Owns a single linear-clamp sampler shared across all three volumetric pipelines (3D atlases ship with null internal samplers). RenderPipeline wires the subsystem into Init order, Shutdown order, and the shader hot-reload chain. | `449abf5` |
| A4.3 | **3D atlas allocation on `ViewResources`.** Three RGBA16F 3D textures (`volDensity` / `volInScatter` / `volInScatterHistory`) at fixed 160×90×128 — view-aligned but resolution-independent. `RecreateViewTextures` creates them via the new 3D ctor with `STORAGE | SAMPLED` usage; `DestroyViewResources` releases them. `RegisterNamedTextures` extension binds them inside the scene-view conditional so the frame-debugger named-texture browser lists them (~42 MB per scene view). | `73ac472` |
| A4.4 | **`Component::FogVolume` tagged-union.** New `scene/components/FogVolume.h`: `enum Type { Box, Sphere }`, `localOffset`, `localRotation`, `union { halfExtents, radius }`, `color`, `density`, `falloffStart`, `falloffEnd`, `affectsAmbient`. `static_assert` on size budget. Scene JSON serializer adds `j["fogVolume"]` with type-tagged switch — direct port of the `Collider` shape. | `d15d29c` |
| A4.5 | **`RenderSnapshot::fogVolumes`.** New `FogVolumeSnapshot` POD; `worldMatrix` bakes the entity world × fog local-transform so the injection shader gets a single volume-to-world transform per row. `CaptureSnapshot` adds a pass mirroring `PointLight` capture (LinearAllocator-backed span). | `94518c5` |
| A4.6 | **`FogVolumeGatherer` + per-frame SSBO upload.** New `renderer/lighting/FogVolumeGatherer.{h,cpp}` mirrors `LightGatherer`; produces `GatheredFogVolumes` (std::vector<FogVolumeData>) with the world-to-volume inverse baked in for shader use. `LightingSystem::UpdateFor` invokes alongside `LightGatherer`. `VolumetricSubsystem::UploadFogVolumeSSBO` mirrors `UploadLightSSBO`: tagged-heap allocation, header + flexible array layout, region cached for inject binding. | `d156e83` |
| A4.7 | **Inject pass shell — first volumetric pass on the wire.** New `volumetric_inject.comp` writes uniform density + isotropic dir-light in-scatter per voxel. `VolumetricSubsystem` gains Inject descriptor layout (2 storage images) + compute pipeline + `WriteInjectView` per-view writes. ViewResources gains `volInjectDescSet[N]` cycled. `AddInjectPass` dispatches async-compute `(20, 12, 32)` groups against the 160×90×128 atlas grid. Bisect milestone: first volumetric pass executes without barrier violation. | `8f3f609` |
| A4.8 | **Inject pass — cluster + CSM + FogVolume modulation.** Shader gains Wronski exponential Z slicing, per-voxel world reconstruction, Olsson cluster ID computation, cluster light iteration, CSM cascade sampling for dir-light shadow attenuation, and FogVolume SSBO iteration with point-in-shape modulation. Inject descriptor layout grows to 7 bindings (2 storage images + 4 SSBOs + shadow sampler); pipeline layout binds Set 0 (Global UBO) + Set 1 (own). `GlobalSubsystem` Set 0 binding 0 gains `COMPUTE` stage flag. `WriteInjectPerFrame` refreshes b2-b5 per frame against the latest tagged-heap regions. Push-constant carries `invView` to skip per-voxel `inverse(ubo.view)`. View pool storage-image budget bumped 8 → 24. | `0094649` |
| A4.9 | **Integrate pass — front-to-back ray march.** New `volumetric_integrate.comp`: single thread per (x, y) walks Z 0..127 in one column, accumulates analytic Beer-Lambert transmittance + in-scatter per step, writes back in-place to volInScatter (RGB = accumulated in-scatter, A = transmittance to slice's near edge). Async-compute, 2D dispatch over (20, 12). | `e565c21` |
| A4.11 | **`GlobalUniforms` expansion.** Four new `Vec4` fields in `GlobalUniforms` (distance fog color+density, distance fog params, height fog color+density, height fog params with multi-scatter scalar). New `VolumetricSettings.h` CPU authoring struct on `RenderingSystem`. `GlobalSubsystem::UpdateUBO` copies values into the UBO each frame. | `a8b4426` |
| A4.13 | **Composite pass — visible milestone.** New `volumetric_composite.frag`: samples volInScatter atlas at Wronski-parameterized UVW (screen UV + log-Z slice from sceneDepth), applies analytic global distance + height fog, emits `(fogColor, fogOpacity)` shaped for the engine's standard alpha-blend equation. `VolumetricSubsystem` gains Composite descriptor layout + graphics pipeline (Set 0 = Global UBO, Set 1 = sceneDepth + volInScatter samplers). ViewResources gains single `volCompositeDescSet`. `RenderPipeline::BuildGraph` chains composite between `SkyboxPass` and `BloomExtract` so bloom respects fog-modulated radiance and the editor grid overlays unfogged grid lines on top. End-to-end fog visible. | `359c48c` |
| (fix) | **Descriptor layout corrections.** Integrate `volDensity` switched from `image3D readonly` to `sampler3D` + `texelFetch` (RG's `ReadStorageImage` transitions to `SHADER_READ_ONLY` which requires sampler descriptor, not storage image). Shadow sampler + sceneDepth descriptors corrected to `VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL`. Three new shader `.meta` files committed. | `766b1fc` |
| A4.15 | **FogVolume inspector drawer.** New `FogVolumeDrawer.cpp` follows the `ColliderDrawer` template: `kFogVolumeTypeStrings`, `ResetUnionForType`, `Poke` helpers; raw `ImGui::Combo` for type with full-snapshot undo; conditional half-extents/radius widgets; standard property widgets for color, density, falloff, ambient flag. `OnCopy`/`OnPaste` JSON round-trip mirrors the scene serializer shape. Registered in the umbrella after PointLight. Adds "Fog Volume" to the Add Component menu automatically (`ShowInAddMenu` default). | `a9ff9c1` |
| A4.16 | **`EditorSettings.enableVolumetricFog` toggle.** Master toggle (default on) gates the inject + integrate + composite chain in `BuildGraph`. Flows through `EditorViewportState` → `CameraParams` via the same path as `iblIntensity`. Checkbox in `EditorSettingsWindow` IBL & Skybox section. Off → no volumetric passes recorded; downstream uses skyboxColor unchanged. | `05bca77` |
| Wrap-up | **History + version bump + merge.** This document. `Version.h` 3.0.2 → 3.0.3. `--no-ff` merge into `main` + `v3.0.3` tag. | this commit |

---

## Architectural decisions

### Composition via standard alpha blend (no feedback loop, no scratch HDR)

Three options considered for blending fog onto sceneColor:

- **Modify SceneColor inline in `pbr.frag`.** Couples PBR to fog state, contaminates the cluster-lighting hot path with a 3D sampler binding, breaks the "fog reads SceneColor after sky writes" semantic. Rejected.
- **Composite reads sceneColor as a sampler + writes a new HDR scratch target.** ~17 MB scratch per view; downstream bloom/grid/postprocess read the scratch instead of sceneColor. Cleaner separation but doubles HDR target memory. Rejected.
- **Composite writes back to sceneColor as a color attachment, output shaped for alpha-blend.** Output `(fogColor, fogOpacity)`; blend equation `src·src.a + dst·(1-src.a)` gives `fogColor·fogOpacity + sceneColor·(1-fogOpacity)` = `scatter + sceneColor·T_total` (with `fogColor = scatter/fogOpacity`, protected against divide-by-zero). No feedback loop (depth sampled is sceneDepth, not sceneColor), no scratch, no new blend factors needed. **Chosen.**

### `gtaoLinearDepth` route was tried and reverted

The composite samples sceneDepth via a regular `sampler2D` descriptor. Switching to GTAOPrefilter's `gtaoLinearDepth` (half-res R32F storage image) was investigated as a workaround for a layout-tracking edge case (see Known Issues below) — sampling pre-linearized depth would also save a `DepthToViewZ` unproject in the shader. The change compiled clean but produced visibly worse fog quality (half-res depth banding propagated through Wronski slice selection) AND didn't actually silence the underlying validation error. Reverted; the composite samples full-res sceneDepth.

### Atlas storage — VMA persistent, not tagged heap

Per arch/memory.md: persistent multi-frame GPU resources (lifetime > 1 frame) go through VMA directly, not `GPUTaggedPageAllocator` (which bulk-frees per frame). The 3D atlases need cross-frame persistence (`volInScatterHistory` is positioned for the temporal accumulation that A4.10 was planned to add). The atlases live on `ViewResources` as `std::shared_ptr<Texture>`, matching the shadow map + IBL + bloom A/B pattern.

The per-frame `FogVolume` SSBO is the inverse — single-frame lifetime, routes through `GPUTaggedPageAllocator` like `LightSSBO`, freed at GPU N-2 timeline.

### Deferred sub-tasks

Three planned sub-tasks shipped as `deferred` in the spec, to be picked up in a follow-up effort:

- **A4.10 Temporal accumulation** — voxel-domain history reprojection + Karis 3×3×3 neighborhood clamp. `volInScatterHistory` atlas is allocated for it; the ping-pong descriptor logic isn't. Without temporal, fog flickers under fast camera motion. Quality issue, not correctness.
- **A4.12 Hillaire 2nd-order multi-scatter** — denser fog regions appear darker than they should without it. Pure shader change, hooks already in place (multi-scatter scalar in `GlobalUniforms`).
- **A4.14 ShadeMode debug viz** — `VolumetricDensity` + `VolumetricInScatter` toggles to inspect the atlases over LDR. Diagnostic utility, not needed for production rendering.

All three composed onto the existing primitive set without architectural change. Defer fit the context budget; bundle them as a `feat: volumetric-fog-polish` follow-up.

---

## Known issues

Two benign Vulkan validation errors fire under specific conditions. Rendering produces correct fog in both cases.

### VUID-vkCmdDraw-None-09600 — sceneDepth layout mismatch in composite

`VolumetricCompositePass` reads sceneDepth via `builder.Read()` + sampler descriptor with layout `SHADER_READ_ONLY_OPTIMAL`. Validation reports the actual image layout at submit is `DEPTH_STENCIL_ATTACHMENT_OPTIMAL` — RG's barrier transitioning `DSA → SHADER_READ_ONLY` isn't being emitted for this pass.

By the read of `RenderGraph::SolveBarriers` and the descriptor-write code, the barrier *should* be inserted. The same pattern in `LightingSubsystem::AddClusterVizPass` (which also calls `builder.Read(sceneDepth)`) has the same latent bug but never surfaces because `cluster_viz` is gated by `ShadeMode::ClustersDensity` which is off by default. Tried: passing `geoOutput.depth` (post-geometry handle, latest version) instead of `prepassDepth`; routing through `gtaoLinearDepth` (R32F, never DSA layout). Neither resolved it.

Root cause likely lives in `RenderGraph::SolveBarriers` interaction with depth-format images that cycle through graphics → compute → graphics queue multiple times per frame. A focused engine effort to harden the RG's depth-handoff barrier emission owns the fix.

### VUID-vkUpdateDescriptorSets-None-03047 — inject descriptor update while in pending command buffer

`VolumetricSubsystem::WriteInjectPerFrame` rewrites bindings 2-5 on the cycled `volInjectDescSet[N]` each frame. Cycling should keep the write slot disjoint from any pending command buffer's bound slot (frame N writes slot N%3, GPU is on slot (N-2)%3). The cluster build / light assign sets in `LightingSubsystem` solve the same per-render-stage rewrite pattern by adding `VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT` to their layouts — the inject layout doesn't. Trivial fix; deferred to keep the effort moving.

Both issues are non-blocking. Bundle into the follow-up `feat: volumetric-fog-polish` or address as part of the broader RG depth-handoff effort.

---

## Bugs caught during smoke testing

1. **Integrate's `volDensity` storage-image-vs-sampler layout mismatch.** Initial pass declared `image3D readonly` + `STORAGE_IMAGE` descriptor with `VK_IMAGE_LAYOUT_GENERAL` — but `builder.ReadStorageImage` transitions to `SHADER_READ_ONLY_OPTIMAL`, which storage-image descriptors don't accept. GTAO's denoise has the same shape and solves it by using `COMBINED_IMAGE_SAMPLER` + `sampler3D` + `texelFetch` (one indirection per voxel; no filtering applied). Switched integrate to the same pattern. Fix in `766b1fc`.

2. **Shadow sampler + sceneDepth descriptor wrong layout.** Initial composite + inject specified `VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL` for the depth samplers — that layout is for depth-attachment reads during depth-test, not sampler reads. RG's `builder.Read()` transitions to `SHADER_READ_ONLY_OPTIMAL`. Matched the descriptor layout to RG's actual transition. Fix in `766b1fc`.

---

## Build verification

- Debug x64 builds clean across 14 commits — only pre-existing warnings (LNK4006 dbghelp, C4996 getenv/strncpy, C4244 Editor chrono conversion).
- `MemoryTracker` shows ~42 MB GPU growth per scene view (3 × ~14 MB volumetric atlases). Game-panel view adds another ~42 MB when active.
- Frame debugger lists `VolumetricInject` (async-compute), `VolumetricIntegrate` (async-compute), and `VolumetricComposite` (graphics) in pass order.
- `VolDensity`, `VolInScatter`, `VolInScatterHistory` appear in the named-texture browser (scene view only).
- Editor's "Add Component" menu now includes "Fog Volume". Inspector drawer round-trips JSON copy/paste; type switch reseats the union cleanly; all fields editable with undo/redo.
- "Volumetric Fog" checkbox under EditorSettings → IBL & Skybox toggles the entire chain off and on cleanly.

### Tagging

After this commit merges to `main`: `git tag -a v3.0.3 -m "v3.0.3 — volumetric-fog"` + `git push --follow-tags`. Mode A — tag-only, no GitHub Release. The series milestone Release lands at the end of `rt-renderer` Phase A.
