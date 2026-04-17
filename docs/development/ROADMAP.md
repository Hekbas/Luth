# Luth Engine — Roadmap

## Completed Phases

| Phase | Summary | Date | Version |
|-------|---------|------|---------|
| 1 — Job System Rewrite | Fiber-based scheduler: FLS, Chase-Lev work-stealing, MPMC queues, SpinLock, isolated main thread | 2026-03-07 | v1.0.0 |
| 2 — Frame Pipeline | Triple-buffered pipelined execution (Game N / Render N-1 / GPU N-2), unified MAX_FRAMES_IN_FLIGHT=3 | 2026-03-07 | v1.0.0 |
| 3 — Render Graph Refactor | DAG compile with dead-pass culling + batched barriers, serial execution with parallel inner recording | 2026-03-07 | v1.0.0 |
| 4 — Cleanup | Hot-path mutex → SpinLock, deleted GLAD/OpenGL remnants, removed temp files | 2026-03-07 | v1.0.0 |
| 5 — Rendering Debug | Fixed 5 rendering bugs: SceneColor disconnect, depth clear, bindless slot 0, Y-flip, front face winding | 2026-03-15 | v1.0.0 |
| 5-A — PBR Shader + Material | Cook-Torrance BRDF, Material SSBO (Set 2), per-RenderMode pipeline variants | 2026-03-15 | v1.0.0 |
| 5-B — Lighting + Shadows | LightUBO (Set 3), ShadowPass (2048² D32), PCF 3×3, ECS-driven light collection | 2026-03-15 | v1.0.0 |
| 5-C — Shader System | ShaderLibrary singleton, hot-reload via FileWatcher, SPIRV-Cross reflection enabled | 2026-03-15 | v1.0.0 |
| 5-H — Inspector + Material Editor | Save button, dirty tracking, Add Component (MeshRenderer/Animation), DirLight shadow controls, albedo color picker | 2026-03-16 | v1.0.0 |
| 5-K — Shader Asset Pipeline | ShaderImporter, SPIR-V artifact cache, stable UUIDs via .meta, fixed AssetDatabase init bugs, MaterialSystem dirty sync | 2026-03-19 | v1.0.0 |
| 5-I — Mipmap Generation | vkCmdBlitImage chain, TextureSettings pipeline (.meta → importer → artifact → VKTexture), sampler maxLod | 2026-03-19 | v1.0.0 |
| 5-J — Scene Serialization | JSON `.luth` format, Win32 file dialogs, editor File menu + shortcuts, dirty tracking, ProjectPanel scene loading | 2026-03-19 | v1.0.0 |
| Asset Lifetime Fix | Scene holds shared_ptrs to prevent GC eviction, MaterialSystem shared_ptr, full async load chain, VMA shutdown fix | 2026-03-21 | v1.0.0 |
| 5-E — Frame Debugger | GPUTimerPool, RenderGraphSnapshot, split-panel UI, event slider, named texture registry | 2026-03-22 | v1.0.0 |
| 5-D — Post-Processing | HDR pipeline (RGBA16F), bloom (extract + Gaussian blur), tonemapping (4 operators), vignette, grain, CA | 2026-03-22 | v1.0.0 |
| 5-F — Pipeline Cache + Variants | VkPipelineCache disk persistence (cache/pipeline.bin), PipelineManager keyed by {shaderUUID, renderMode} with lazy creation and targeted hot-reload invalidation | 2026-03-22 | v1.0.0 |
| 5-G — Skybox + IBL | HDR equirect→cubemap, irradiance convolution, pre-filtered env map (5 mips), BRDF LUT, PBR split-sum ambient, skybox pass (depth=1.0 trick), Set 0 expanded to 4 bindings | 2026-03-23 | v1.0.0 |
| 6 — Polish & Editor QOL | Bug fixes (transform dispatch, alpha cutoff, shadows), Rider theme, inspector overhaul, editor persistence/layouts, mouse picking + selection outline + shade modes, profiler rework | 2026-03-25 | v1.0.0 |
| 6B — Editor QA Triage | 22-item QA pass: semantic EditorColors, skybox HDR picker, outline children+occluded-fade, recursive hierarchy search, Point Light/Camera context menu entries, primitive geometry creation, project panel folder search, eager scene dep loading, resource panel Font/Scene filters + type icons + column sorting + table polish | 2026-03-30 | v1.0.0 |
| 7 — Animation System | Fiber-parallel keyframe sampling, GPU skinning (BoneMatrixBuffer SSBO), SQT blending, crossfade transitions, layered override with bone masks, root motion extraction, bone debug overlay, full editor inspector integration | 2026-03-30 | v1.0.0 |
| 8 — Smart Import & Hot Reload | Multi-strategy texture discovery (4 search strategies), ImportReport + TextureRemapDialog, Project Panel hot reload via FileWatcher (1 s polling, Created/Modified/Deleted), drop-to-current-dir, eager texture copy + import on drop, TOCTOU crash fix in FileWatcher, Assets menu "Resolve Missing Textures..." | 2026-03-31 | v1.0.0 |
| 9 — Frame Debugger Upgrade | Trigger-based capture, per-draw-call scrubbing, DebuggerState machine (Inactive→CaptureRequested→Frozen), full pass instrumentation, RenderCapturedFrame re-recording, rescue blit for truncated frames, depth linearization shader, rewritten panel UI | 2026-04-03 | v1.0.0 |
| 10 — Undo/Redo History | Command pattern with 14 command types, UUID-based entity resolution, gizmo drag coalescing, compound commands, material snapshot undo, HistoryPanel debug UI, 3 stale-handle crash fixes | 2026-04-09 | v1.1.0 |
| 11 — Architecture Cleanup | RenderingSystem split (4,060→2,321 LOC): EditorCamera extraction, CameraParams decoupling, IBLPrecompute, FrameDebugger class, 9 render passes to `renderer/passes/`, Command.h modularized into `editor/commands/` (6 sub-headers) | 2026-04-13 | v1.1.1 |
| 12 — Compute + GPU Culling | Render graph compute pass + buffer support, `VulkanComputePipeline`, GPU frustum cull compute shader, GPUObjectData SSBO (Set 5), all draw passes converted to `vkCmdDrawIndexedIndirect`, IBLPrecompute refactored, Frame Debugger extended with indirect + compute capture | 2026-04-15 | v1.2.0 |
| 13 — Cascaded Shadow Maps | 4-cascade PSSM (Sascha Willems bounding-sphere fit), 4-layer shadow array, per-cascade GPU cull, cascade selection + blending + bias in PBR shader, cascade debug viz | 2026-04-16 | v1.3.0 |
| 14 — Frame Debugger Sync Rework | Archive sink + per-pass image staging, frozen-state model with auto-recapture on camera move, hierarchical EventNode tree (Group/Pass/Cascade/Draw), per-draw replay-then-copy for GeometryPass, CSM cascade detail panel + linearized depth preview, deferred archive teardown | 2026-04-17 | v1.4.0 |

> For detailed writeups of each phase (narratives, root causes, fixes, files modified), see individual files in [`history/`](history/).

---

## Planned Phases

| Priority | Phase | Epic | Version | Est. Time | Dependencies |
|----------|-------|------|---------|-----------|-------------|
| 3 | 15 — GTAO | #58 | v1.5.0 | 1-2 weeks | Phase 12 |
| 4 | 16 — Play Mode | #66 | v1.6.0 | 1-2 weeks | — |
| 5 | 17 — Jolt Physics | #56 | v1.7.0 | 2-3 weeks | Phase 16 |
| 6 | 18 — Jiggle Bones | #61 | v1.7.1 | 1 week | — |
| 7 | 19 — Forward+ Lighting | #54 | v1.8.0 | 2 weeks | Phase 12 |
| 8 | 20 — FXAA / TAA | #72 | v1.8.1 | 1 week | — |
| 9 | 21 — GPU Particles | #57 | v1.9.0 | 2-3 weeks | Phase 12, 19 |

> Full specs and dependency graph: [`ROADMAP_TODO.md`](ROADMAP_TODO.md)

---

## Versioning

Semantic Versioning (`MAJOR.MINOR.PATCH`):
- **MAJOR** — fundamental architecture changes or engine rewrites
- **MINOR** — each completed development phase
- **PATCH** — bug fixes and polish between phases

Version is centralized in `luth/source/luth/core/Version.h`.

## Planned Versions

| Version | Phase(s) | Status |
|---------|----------|--------|
| v1.0.0  | Phases 1–9 (engine rewrite) | Released 2026-04-05 |
| v1.1.0  | Phase 10 (undo/redo history) | Released 2026-04-09 |
| v1.1.1  | Phase 11 (architecture cleanup) | Released 2026-04-13 |
| v1.2.0  | Phase 12 (compute + GPU culling) | Released 2026-04-15 |
| v1.3.0  | Phase 13 (cascaded shadow maps) | Released 2026-04-16 |
| v1.4.0  | Phase 14 (frame debugger sync rework) | Released 2026-04-17 |

---

## Future Ideas (Not Scoped)

> Items covered by Planned Phases (12-20) are tracked there. Below are ideas beyond Phase 20.

### Rendering
- Deferred GBuffer rendering
- Global illumination (screen-space or probe-based)
- Volumetric fog / haze
- SSR (screen-space reflections)
- HZB occlusion culling

### Gameplay Systems
- Animation Phase 8: DQS, morph targets, IK, state machines, animation LODs
- Prefab system (reusable entity templates)
- Scripting (C# via Mono, or Lua)

### Audio
- 3D spatial audio
- Audio asset pipeline

### Editor & Tools
- Asset streaming (async GPU upload pipeline)
- Visual shader editor
