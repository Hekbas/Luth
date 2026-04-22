# Luth Engine — Architecture Reference

> For progress tracking and upcoming work, see [`ROADMAP.md`](ROADMAP.md).

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

## Build Targets

Since `arch-target-split` (v2.0.0) the engine and editor are separate static libs:

| Target | Kind | Links | Contents |
|--------|------|-------|----------|
| `Luth.lib`    | StaticLib  | —             | Engine only — no ImGui panels, no editor classes |
| `Luthien.lib` | StaticLib  | Luth          | Editor — panels, inspectors, commands, style, widgets, `LuthienEditorHooks` impl |
| `Luthien.exe` | ConsoleApp | Luth, Luthien | Thin runtime at `runtime/` — `LuthienApp` subclass; `CreateApp()` installs editor hooks before app ctor |

Engine → editor calls route through the nullptr-safe `Luth::EditorHooks` interface in `luth/core/EditorHooks.h`. A runtime-only host can link `Luth.lib` alone; every engine-side hook call short-circuits when the registry is empty.

## System Hierarchy

```text
[Luth Engine — Luth.lib]
 │
 ├── [Core]
 │    ├── JobSystem .............. N:M Fiber Scheduler (FLS, Chase-Lev, MPMC)
 │    ├── Memory ................. TaggedPageAllocator + LinearAllocator
 │    ├── IOThread ............... Dedicated OS thread for disk I/O
 │    ├── App .................... Two-phase init: Engine boot → Project load
 │    ├── EntryPoint, Version, FrameData, UUID, ProjectFile, EditorHooks .. top-level lifecycle (core/ root)
 │    ├── types/                 LuthTypes (primitives), LuthMath (Vec/Mat/Quat aliases + `Math::` facade + Assimp/AABB/Frustum), TypeTraits (IsGLMVector/Matrix)
 │    ├── diagnostics/           Log, LogFormatters (fmt + ostream<< Vec3/Mat4), Profiler
 │    └── time/                  Time, Timer
 │
 ├── [Events]
 │    └── EventBus ............... Deferred queue-swap dispatch (AppEvent, KeyEvent, MouseEvent, RenderEvent, FileDropEvent)
 │
 ├── [Platform]
 │    ├── Window / WinWindow ..... GLFW window + Win32 dark-mode title bar
 │    ├── Input .................. Keyboard/mouse state (queries editor capture via EditorHooks)
 │    └── FileDialog ............. Native open/save dialogs
 │
 ├── [Renderer (Vulkan 1.3)]
 │    ├── RenderGraph ............ DAG compile → barrier inject → execute
 │    ├── RenderPipeline ......... ~780-LOC orchestrator — Initialize / Shutdown / Execute / OnResize / CaptureSnapshot; Init* / Update* helpers + CreatePipelines live in sibling topic files
 │    ├── FrameTargets ........... SceneColor / SceneDepth / EntityID / LDROutput / Selection {mask,depth}
 │    ├── DrawListBuilder ........ ECS walk → opaque/cutout/transparent buckets
 │    ├── Backend ................ VulkanContext, VulkanBackend, Timeline Semaphores
 │    ├── passes/ ................ Shadow, DepthPrepass, AO (GTAO + AOInit), Cull, Geometry, Selection, Skybox, Grid, Bloom, PostProcess, Outline, ImGui
 │    ├── lighting/ .............. LightGatherer, CascadeBuilder, IBLPrecompute, LightTypes, ShadowInit, IBLInit
 │    ├── postprocess/ ........... PostProcessInit (bloom textures + PP UBO + outline/grid descriptors)
 │    ├── gpu/ ................... GPUObjectBuffers (object SSBO + indirect buffer + cull pipeline + per-frame fill)
 │    ├── resources/ ............. Texture, Mesh, Model, Buffer, Skeleton, AnimationClip, BoneMatrixBuffer (Set 4 bone SSBO), GlobalUniforms
 │    ├── material/ .............. Material, MaterialSystem
 │    ├── shader/ ................ Shader, ShaderCompiler, ShaderLibrary, ShaderWatcher (hot-reload service owned by RenderPipeline)
 │    ├── pipeline/ .............. PipelineManager, PipelineFactory (8 per-family pipeline builders: PBR / Shadow / DepthPrepass / Selection / Skybox / Post / Outline / Grid)
 │    ├── settings/ .............. GTAOSettings, PostProcessSettings
 │    ├── draw/ .................. DrawCommand
 │    ├── debug/ ................. FrameDebuggerContext (preview textures + blit pass + per-draw replay-then-copy + depth archive blit)
 │    ├── FrameDebugger .......... Archive + state machine (owned by RenderingSystem); render-side infrastructure in debug/FrameDebuggerContext
 │    └── Renderer ............... High-level BeginFrame/EndFrame façade
 │
 ├── [Scene / ECS]
 │    ├── Scene, Entity
 │    ├── components/ ............ Granular headers (Common, Transform, Camera, Rendering, Lights, Animation, AnimationController); Components.h umbrella preserved
 │    └── SystemRegistry ......... vector<unique_ptr<ISystem>>, Update<T>() dispatch
 │         ├── TransformSystem ... Parallel level-based hierarchy propagation
 │         ├── AnimationSystem ... Fiber-parallel keyframe sampling, GPU skinning
 │         ├── CameraSystem ..... Projection + view matrix computation per frame
 │         ├── LightingSystem ... Light gather + CSM cascade fit (LightGatherer + CascadeBuilder); outputs consumed by RenderPipeline
 │         ├── RenderingSystem .. ~200-LOC ECS→DrawList dispatcher; owns FrameTargets + CameraParams + FrameDebugger + DrawList; invokes RenderPipeline::Execute
 │         └── PickingSystem .... Mouse-pick readback from EntityID buffer; runs post-render
 │
 └── [Asset Pipeline (resources/)]
      ├── AssetDatabase .......... Two-phase: InitEngine() → LoadProject()
      ├── AssetManager ........... Async loading, GPU upload queue, GC
      ├── Importers (Model, Texture, Shader, Material)
      └── FileSystem ............. Dual-root: engine assets + project assets

[Luthien Editor — Luthien.lib, at luthien/source/luthien/]  (details: arch/editor.md)
 │
 ├── Editor, UI, EditorSelection, EditorCamera, EditorSettings, EditorStyle
 ├── Bootstrap.h / EditorHooks.cpp ─ InstallLuthienEditorHooks() forwards IEditorHooks → Editor::*
 ├── CommandHistory + commands/ ... Undo/redo: ICommand + 14 command types, compound recording
 ├── ProjectLauncher ............. Startup project selector, recent projects
 ├── panels/ ..................... Scene, Hierarchy, Inspector, Project, Render, FrameDebugger, Profiler, History, TextureRemapDialog
 ├── inspectors/ ................. MaterialEditor, ModelViewer, TextureEditor, ShaderEditor, SceneViewer, FontViewer
 └── widgets/ .................... Icons (FontAwesome defs), ImGuiUtils
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
| Shadows | `ShadowPass` (2048² D32) + 4-cascade PSSM (Sascha Willems bounding-sphere fit), PCF 3×3 via `sampler2DShadow` |
| AO | GTAO half-res compute chain (prefilter → horizon integral → bilateral denoise), Jimenez 2016 slice integral |
| GPU culling | Compute frustum cull per shadow cascade + main scene, `GPUObjectData` SSBO (Set 5), `vkCmdDrawIndexedIndirect` |
| Animation | Fiber-parallel keyframe sampling, GPU skinning via `BoneMatrixBuffer` SSBO (Set 4), SQT blending, crossfade, layered override, root motion |
| Post-processing | HDR (RGBA16F), bloom, tonemapping (4 operators), vignette, grain, CA |
| Shader system | Single-stage shader assets (.vert/.frag/.comp each one artifact + UUID); `ShaderLibrary::LoadEngine` routes engine shaders through the asset pipeline; hot-reload via `FileWatcher` on any stage; SPIRV-Cross reflection |
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
| Animation (clips, skeleton, GPU skinning, blending) | [`arch/animation-system.md`](arch/animation-system.md) | Working on animation |
| Editor (panels, IEditorHooks, selection, UI) | [`arch/editor.md`](arch/editor.md) | Working on editor |
