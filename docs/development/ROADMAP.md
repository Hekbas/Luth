# Luth Engine — Roadmap

## Completed Phases

| Phase | Summary | Date |
|-------|---------|------|
| 1 — Job System Rewrite | Fiber-based scheduler: FLS, Chase-Lev work-stealing, MPMC queues, SpinLock, isolated main thread | 2026-03-07 |
| 2 — Frame Pipeline | Triple-buffered pipelined execution (Game N / Render N-1 / GPU N-2), unified MAX_FRAMES_IN_FLIGHT=3 | 2026-03-07 |
| 3 — Render Graph Refactor | DAG compile with dead-pass culling + batched barriers, serial execution with parallel inner recording | 2026-03-07 |
| 4 — Cleanup | Hot-path mutex → SpinLock, deleted GLAD/OpenGL remnants, removed temp files | 2026-03-07 |
| 5 — Rendering Debug | Fixed 5 rendering bugs: SceneColor disconnect, depth clear, bindless slot 0, Y-flip, front face winding | 2026-03-15 |
| 5-A — PBR Shader + Material | Cook-Torrance BRDF, Material SSBO (Set 2), per-RenderMode pipeline variants | 2026-03-15 |
| 5-B — Lighting + Shadows | LightUBO (Set 3), ShadowPass (2048² D32), PCF 3×3, ECS-driven light collection | 2026-03-15 |
| 5-C — Shader System | ShaderLibrary singleton, hot-reload via FileWatcher, SPIRV-Cross reflection enabled | 2026-03-15 |
| 5-H — Inspector + Material Editor | Save button, dirty tracking, Add Component (MeshRenderer/Animation), DirLight shadow controls, albedo color picker | 2026-03-16 |
| 5-K — Shader Asset Pipeline | ShaderImporter, SPIR-V artifact cache, stable UUIDs via .meta, fixed AssetDatabase init bugs, MaterialSystem dirty sync | 2026-03-19 |
| 5-I — Mipmap Generation | vkCmdBlitImage chain, TextureSettings pipeline (.meta → importer → artifact → VKTexture), sampler maxLod | 2026-03-19 |
| 5-J — Scene Serialization | JSON `.luth` format, Win32 file dialogs, editor File menu + shortcuts, dirty tracking, ProjectPanel scene loading | 2026-03-19 |
| Asset Lifetime Fix | Scene holds shared_ptrs to prevent GC eviction, MaterialSystem shared_ptr, full async load chain, VMA shutdown fix | 2026-03-21 |
| 5-E — Frame Debugger | GPUTimerPool, RenderGraphSnapshot, split-panel UI, event slider, named texture registry | 2026-03-22 |
| 5-D — Post-Processing | HDR pipeline (RGBA16F), bloom (extract + Gaussian blur), tonemapping (4 operators), vignette, grain, CA | 2026-03-22 |
| 5-F — Pipeline Cache + Variants | VkPipelineCache disk persistence (cache/pipeline.bin), PipelineManager keyed by {shaderUUID, renderMode} with lazy creation and targeted hot-reload invalidation | 2026-03-22 |
| 5-G — Skybox + IBL | HDR equirect→cubemap, irradiance convolution, pre-filtered env map (5 mips), BRDF LUT, PBR split-sum ambient, skybox pass (depth=1.0 trick), Set 0 expanded to 4 bindings | 2026-03-23 |
| 6 — Polish & Editor QOL | Bug fixes (transform dispatch, alpha cutoff, shadows), Rider theme, inspector overhaul, editor persistence/layouts, mouse picking + selection outline + shade modes, profiler rework | 2026-03-25 |

> For detailed writeups of each phase (narratives, root causes, fixes, files modified), see individual files in [`history/`](history/).

---

## Current Phase: 7 — Animation System

| Sub-Phase | Summary | Status |
|-----------|---------|--------|
| 7A — Data Extraction | Skeleton, AnimationClip, SkinnedVertex structs. Assimp extraction (skeleton hierarchy, bone weights, keyframes). V2 binary serialization. ModelViewer populated from real data. | Complete (2026-03-27) |
| 7B — GPU Skinning | BoneMatrixBuffer SSBO (Set 4, 128×256 mat4, 2MB), pbr_skinned.vert + shadowDepth_skinned.vert with LBS, 5-set pipeline layout, dual PipelineManagers (static/skinned), per-mesh skinning detection. Skinned meshes render in bind pose. | Complete (2026-03-27) |
| 7C — Evaluation & Playback | AnimationSystem with fiber-parallel keyframe sampling (binary search + lerp/slerp), skeleton hierarchy propagation, per-entity bone block ownership (moved from RenderingSystem), SSBO upload. AABB struct + bind-pose/animated bounding boxes. Animation events with crossing detection (loop wrap-around). BoneAttachment component with lazy name resolution, post-eval transform write, serialization with deferred UUID resolution. | Complete (2026-03-28) |
| 7D — Blending & Root Motion | AnimationController component with SQT-space blending. Crossfade transitions (both clips advance, alpha ramp). Layered override with per-bone masks. Root motion extraction (XZ delta from root bone track, loop-wrap two-segment handling, main-thread application with entity rotation). PropagateAndUpload shared tail extracted from single-clip path. Full serialization (layers, sparse bone masks, root motion flag). Editor inspector (layer list, clip/weight/speed/loop per layer, bone mask checkboxes, Add Component entry). Backward compatible — entities without controller use unchanged 7C path. | Complete (2026-03-30) |
| 7E — Editor Integration | Animation inspector (clip selector, transport, timeline scrubber, frame counter), BoneAttachment inspector, bone debug overlay, model instantiation with Animation component and bone hierarchy entities, ModelViewer animation table. Fixed AnimationSystem/RenderingSystem parent-child traversal, Assimp $AssimpFbx$ skeleton extraction, SceneSerializer Playing state. | Complete (2026-03-29) |

---

## Future Ideas (Not Scoped)

### Rendering
- Deferred GBuffer rendering
- SSAO (requires GBuffer)
- FXAA / TAA
- Global illumination (screen-space or probe-based)
- Volumetric fog / haze
- Cascaded shadow maps

### Gameplay Systems
- Physics engine (Jolt integration, jobified)
- Particle system (GPU compute)
- Animation Phase 8: DQS, morph targets, IK, state machines, animation LODs
- Prefab system (reusable entity templates)
- Scripting (C# via Mono, or Lua)

### Audio
- 3D spatial audio
- Audio asset pipeline

### Editor & Tools
- Asset streaming (async GPU upload pipeline)
- Undo/redo system
- Play mode (runtime simulation in editor)
