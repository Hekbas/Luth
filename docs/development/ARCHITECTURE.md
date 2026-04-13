# Luth Engine — Architecture Reference

> For full project context, conventions, and build instructions, see `CLAUDE.md` at the repo root.
> For progress tracking and upcoming work, see `ROADMAP.md`.

---

## Design Philosophy

1. **No OS blocking on worker threads.** Fibers yield to the scheduler; OS threads always stay busy.
2. **No `thread_local`.** Fiber Local Storage (FLS) carried in `JobContext`.
3. **No `std::mutex` in the hot path.** Spin-locks (< 100 cycles) or lock-free structures only.
4. **No `new`/`delete` in gameplay/render.** Use `LinearAllocator` (frame) or `TaggedPageAllocator` (tagged lifetime).
5. **No `VkRenderPass`/`VkFramebuffer`.** Dynamic Rendering only (`vkCmdBeginRendering`).
6. **No `vkWaitForFences`.** Timeline Semaphores polled by `VulkanWaitJob`.
7. **Pipelined execution.** Game(N) | Render(N-1) | GPU(N-2).
8. **Main thread is isolated.** OS message pump + present only. Never steals jobs.

> For detailed fiber hazard analysis (V1-V6 vulnerability mitigations), see [`arch/fiber-system.md`](arch/fiber-system.md).

---

## System Hierarchy

```text
[Luth Engine]
 │
 ├── [Core]
 │    ├── JobSystem .............. N:M Fiber Scheduler (FLS, Chase-Lev, MPMC)
 │    ├── Memory ................. TaggedPageAllocator + LinearAllocator
 │    ├── FrameData .............. Triple-buffered FrameContext
 │    ├── IOThread ............... Dedicated OS thread for disk I/O
 │    ├── EventBus ............... Deferred queue-swap dispatch
 │    ├── ProjectFile ............ .luthproj loader/saver, CLI discovery
 │    └── App .................... Two-phase init: Engine boot → Project load
 │
 ├── [Renderer (Vulkan 1.3)]
 │    ├── RenderGraph ............ DAG Compile → Barrier Inject → Execute
 │    ├── Backend ................ VulkanContext, VulkanBackend, Timeline Semaphores
 │    ├── Passes ................. 9 extracted pass files (Shadow, Geometry, Skybox, Bloom, PostProcess, Selection, Outline, Grid, ImGui)
 │    ├── FrameDebugger .......... Capture state machine, per-draw scrubbing, debug blit
 │    ├── IBLPrecompute .......... Equirect→cubemap, irradiance, prefiltered env, BRDF LUT
 │    ├── Material/Shader/Texture/Buffer/Mesh
 │    └── Renderer ............... High-level BeginFrame/EndFrame
 │
 ├── [Scene / ECS]
 │    ├── Scene, Entity, Components (EnTT)
 │    └── Systems (Transform, Camera, Rendering, Animation)
 │
 ├── [Asset Pipeline]
 │    ├── AssetDatabase .......... Two-phase: InitEngine() → LoadProject()
 │    ├── AssetManager ........... Async loading, GPU upload queue, GC
 │    ├── Importers (Model, Texture, Shader, Material)
 │    └── FileSystem ............. Dual-root: engine assets + project assets
 │
 └── [Editor]
      ├── Editor, UI, EditorSelection
      ├── EditorCamera ........... Extracted from ScenePanel (orbit/fly, input, frustum)
      ├── commands/ .............. Undo/redo: ICommand, EntityCommands, ComponentCommands, AssetCommands, ComponentPropertyCommand
      ├── CommandHistory ......... Execute/Undo/Redo stacks, compound recording
      ├── ProjectLauncher ........ Startup project selector, recent projects
      └── Panels (Scene, Hierarchy, Inspector, Project, Render, FrameDebugger, History)
```

> For pipelined frame execution details, see [`arch/frame-pipeline.md`](arch/frame-pipeline.md).

---

## Rendering — Current Baseline

| System | State |
|--------|-------|
| RenderGraph | DAG compile, barrier injection, dead-pass cull, serial execution |
| GeometryPass | PBR forward pass — 3 pipeline variants (Opaque/Cutout/Transparent) |
| Shader | Cook-Torrance BRDF (GGX + Smith + Fresnel-Schlick) |
| Material | `GPUMaterialData` SSBO (Set 2), 9 map types, JSON serialization |
| Lighting | `LightUBO` (Set 3): 1 directional + 64 point lights from ECS |
| Shadows | `ShadowPass` (2048² D32), PCF 3×3 via `sampler2DShadow` |
| Post-processing | HDR (RGBA16F), bloom, tonemapping (4 operators), vignette, grain, CA |
| Shader system | `ShaderLibrary` singleton, hot-reload via `FileWatcher`, SPIRV-Cross reflection |
| Frame Debugger | GPU timers, pass tree, pipeline state, texture preview |
| Mipmaps | `vkCmdBlitImage` chain, per-texture `.meta` settings |
| Scene serialization | JSON `.luth` format, native file dialogs |
| Pipeline cache | VkPipelineCache disk persistence + PipelineManager (lazy, keyed by shader+mode) |
| Skybox / IBL | HDR equirect→cubemap, irradiance convolution, pre-filtered env (5 mips), BRDF LUT, split-sum ambient |
| AA | Not started |

> For descriptor set layout, pass order, and memory budget, see [`arch/rendering-pipeline.md`](arch/rendering-pipeline.md).

---

## Detailed Architecture References

| Area | File | When to read |
|------|------|--------------|
| Fiber system (V1-V6 mitigations) | [`arch/fiber-system.md`](arch/fiber-system.md) | Working on jobs/core |
| Rendering pipeline (descriptor sets, pass order) | [`arch/rendering-pipeline.md`](arch/rendering-pipeline.md) | Working on renderer |
| Frame pipeline (triple-buffer model) | [`arch/frame-pipeline.md`](arch/frame-pipeline.md) | Working on frame pipeline |
| Asset pipeline (importers, loading, caching) | [`arch/asset-pipeline.md`](arch/asset-pipeline.md) | Working on assets/resources |
| Scene & ECS (components, systems, serialization) | [`arch/scene-ecs.md`](arch/scene-ecs.md) | Working on scene/ECS |
| Editor (panels, selection, UI) | [`arch/editor.md`](arch/editor.md) | Working on editor |
