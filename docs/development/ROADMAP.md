# Luth Engine — Roadmap

## Completed Phases

| Phase | Summary | Date       |
|-------|---------|------------|
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
| 6B — Editor QA Triage | 22-item QA pass: semantic EditorColors, skybox HDR picker, outline children+occluded-fade, recursive hierarchy search, Point Light/Camera context menu entries, primitive geometry creation, project panel folder search, eager scene dep loading, resource panel Font/Scene filters + type icons + column sorting + table polish | 2026-03-30 |
| 7 — Animation System | Fiber-parallel keyframe sampling, GPU skinning (BoneMatrixBuffer SSBO), SQT blending, crossfade transitions, layered override with bone masks, root motion extraction, bone debug overlay, full editor inspector integration | 2026-03-30 |
| 8 — Smart Import & Hot Reload | Multi-strategy texture discovery (4 search strategies), ImportReport + TextureRemapDialog, Project Panel hot reload via FileWatcher (1 s polling, Created/Modified/Deleted), drop-to-current-dir, eager texture copy + import on drop, TOCTOU crash fix in FileWatcher, Assets menu "Resolve Missing Textures..." | 2026-03-31 |
| 9 — Frame Debugger Upgrade | Trigger-based capture, per-draw-call scrubbing, DebuggerState machine (Inactive→CaptureRequested→Frozen), full pass instrumentation, RenderCapturedFrame re-recording, rescue blit for truncated frames, depth linearization shader, rewritten panel UI | 2026-04-03 |

> For detailed writeups of each phase (narratives, root causes, fixes, files modified), see individual files in [`history/`](history/).

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
