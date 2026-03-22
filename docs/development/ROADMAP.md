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

> For detailed writeups of each phase (narratives, root causes, fixes, files modified), see individual files in [`history/`](history/).

---

## Upcoming Phases

### Phase 5-F — Pipeline Cache + Variants
Avoid redundant pipeline compilation, support runtime pipeline variants keyed by `{shaderUUID, renderMode}`.

> Full spec: [`phases/5F-pipeline-cache.md`](phases/5F-pipeline-cache.md)

### Phase 5-G — Skybox + IBL
IBL-compatible skybox, cubemap import (equirectangular HDR → cubemap), irradiance/pre-filtered env maps, BRDF LUT.

> Full spec: [`phases/5G-skybox-ibl.md`](phases/5G-skybox-ibl.md)

### Recommended Order

```
5-F (Pipeline Cache) → 5-G (Skybox + IBL)
```

**Rationale:** Pipeline cache is quick and improves startup time. Skybox/IBL requires mipmaps (done) and PBR shader (done).

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
- Animation system improvements (blend trees, IK)
- Prefab system (reusable entity templates)
- Scripting (C# via Mono, or Lua)

### Audio
- 3D spatial audio
- Audio asset pipeline

### Editor & Tools
- Asset streaming (async GPU upload pipeline)
- Undo/redo system
- Play mode (runtime simulation in editor)
