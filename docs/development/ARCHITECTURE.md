# Luth Engine — Architecture Reference

> For progress tracking and upcoming work, see [`ROADMAP.md`](ROADMAP.md).

---

## Design Philosophy

1. **No OS blocking on worker threads.** Fibers yield to the scheduler; OS threads always stay busy.
2. **No `thread_local`.** Fiber Local Storage (FLS) carried in `JobContext`.
3. **No `std::mutex` in the hot path.** Spin-locks (< 100 cycles) or lock-free structures only.
4. **No `new`/`delete` in gameplay/render.** Use `LinearAllocator` (frame) or `TaggedPageAllocator` / `GPUTaggedPageAllocator` (tagged lifetime — Onion/Garlic split).
5. **No `VkRenderPass`/`VkFramebuffer`.** Dynamic Rendering only (`vkCmdBeginRendering`).
6. **No `vkWaitForFences`.** Timeline Semaphores; per-frame completion via `VulkanBackend::IsFrameComplete`.
7. **Pipelined execution.** Game(N) | Render(N-1) | GPU(N-2). Realized in v2.8.4 ([history](history/v2.x/pipeline-phase-3.md)) — game + render dispatch concurrently on worker fibers, handoff via `RenderSnapshot` captured at end of game stage.
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
 │    ├── Memory ................. TaggedPageAllocator (CPU) + GPUTaggedPageAllocator (GPU) + LinearAllocator
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
 ├── [Renderer (Vulkan 1.3 + hardware RT)]
 │    ├── RenderPipeline ......... ~650-LOC orchestrator — holds the subsystem instances, dispatches Init/Shutdown/Update/Add*Pass in dependency order, owns per-view scratch + named-texture registry + shader-reload dispatch
 │    ├── rendergraph/ ........... RenderGraph (DAG compile → barrier inject → execute) + FrameCapture / ArchivedImage / RenderResourceCache
 │    ├── subsystems/ ............ Per-Set / per-feature lifecycle hosts. Raster: Global, Lighting, Geometry, GTAO, PostProcess, EditorOverlays, Skinning, DebugDraw. RT: Rt (BLAS/TLAS), RtRestir (DI), RtRestirGi (GI), Reflections, PathTrace. Denoise/atmos: SvgfDenoiser (IDenoiser), Volumetric
 │    ├── FrameTargets ........... SceneColor / SceneDepth / slim G-buffer (normal/roughness/motion/matID) / EntityID / LDROutput / Selection {mask,depth}
 │    ├── DrawListBuilder ........ ECS walk → opaque/cutout/transparent buckets
 │    ├── TaaJitter / QueueRecorders / CameraParams .. per-view Halton jitter, per-queue cmd recorders, camera POD
 │    ├── Backend ................ VulkanContext (RT extensions + PFN cache), VulkanBackend, Timeline Semaphores
 │    ├── lighting/ .............. LightGatherer, CascadeBuilder, FogVolumeGatherer, IBLPrecompute, LightTypes
 │    ├── resources/ ............. Texture, Mesh, Model, Buffer, Skeleton, AnimationClip, BoneMatrixBuffer (Set 4), GlobalUniforms, per-view ViewResources (RT/denoise/volumetric atlases)
 │    ├── material/ .............. Material, MaterialSystem (GPUMaterialData SSBO, Set 2)
 │    ├── shader/ ................ Shader, ShaderCompiler, ShaderLibrary, ShaderWatcher (hot-reload service owned by RenderPipeline)
 │    ├── pipeline/ .............. PipelineManager (lazy variant creation, disk-persisted VkPipelineCache, keyed by shader+mode)
 │    ├── settings/ .............. GTAO, PostProcess, Volumetric, Restir (DI), RestirGi, Svgf, Reflections, PathTrace
 │    ├── draw/ .................. DrawCommand
 │    ├── passes/ ................ ImGuiPass (remaining standalone pass; the rest moved into subsystems/)
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
 └── widgets/ .................... Icons (semantic map → Phosphor), ImGuiUtils
```

> For pipelined frame execution details, see [`arch/frame-pipeline.md`](arch/frame-pipeline.md).

---

## Rendering — Current Baseline

| System | State |
|--------|-------|
| RenderGraph | DAG compile, barrier injection, dead-pass cull, serial execution; per-pass secondary-cmd recording |
| Geometry | Clustered Forward+ PBR (Olsson log-slice clusters); slim G-buffer prepass (normal RG16F / roughness R8 / motion RG16F / matID R16U); 3 render-mode variants |
| Shader | Slang-only stack (one compiler, reflection-driven); Cook-Torrance BRDF shared raster/RT eval via `common/brdf.slang` |
| Material | `GPUMaterialData` SSBO (Set 2), 8 bindless maps + flag bitfield, JSON `.mat` |
| Lighting | Clustered Forward+ — 1 directional + clustered point lights; per-view cluster grid + light-index SSBOs (Set 3) |
| Shadows | RT ray-query sun shadows (default) + per-view mask; 4-cascade PSSM CSM retained as an A/B `ShadowingMode` toggle |
| RT foundation | KHR ray query + acceleration structure (RT-mandatory); per-mesh BLAS (static + skinned refit), per-frame TLAS rebuild on async compute; bindless geometry table (`instanceCustomIndex` → BDA deref) |
| RT global illumination | ReSTIR DI (Bitterli 2020) + ReSTIR GI (Ouyang 2021) — device-local reservoir reuse, reconnection Jacobian, demodulated + denoised |
| RT reflections | Stochastic GGX-VNDF specular rays from the slim G-buffer + dedicated specular denoiser; supersedes SSR |
| Denoising | SVGF (Schied 2017) — three channel-parameterized instances (DI / GI / specular) behind an `IDenoiser` interface |
| Path tracing | Reference path-traced mode (`RenderMode::PathTrace`) — rayQuery megakernel, multi-bounce NEE + GGX-VNDF lobe MIS, progressive fp32 accumulation |
| Volumetric fog | Wronski froxel grid — density/scatter inject → integrate → temporal resolve → composite; optional per-froxel RT fog shadows |
| AO | GTAO half-res compute chain (prefilter → horizon integral → bilateral denoise), Jimenez 2016 slice integral |
| GPU culling | Compute frustum cull per shadow cascade + main scene, `GPUObjectData` SSBO (Set 5), `vkCmdDrawIndexedIndirect` |
| Animation | Fiber-parallel keyframe sampling, GPU skinning via `BoneMatrixBuffer` SSBO (Set 4), SQT blending, crossfade, layered override, root motion |
| AA | TAA (Karis14 YCoCg-clip recipe) + specular AA (Tokuyoshi 2019) |
| Post-processing | HDR (RGBA16F), bloom, tonemapping (ACES variants + AgX / AgX Punchy), vignette, grain, CA |
| Bindless | BDA enabled; Set 1 — 16384-texture array + 32-slot sampler array (UPDATE_AFTER_BIND, partially-bound); integer material/texture indices |
| Shader system | Single-stage shader assets (.vert/.frag/.comp each one artifact + UUID); `ShaderLibrary::LoadEngine` routes engine shaders through the asset pipeline; hot-reload via `FileWatcher` on any stage; SPIRV-Cross reflection |
| Frame Debugger | GPU timers, pass tree, pipeline state, per-draw replay, texture preview |
| Mipmaps | `vkCmdBlitImage` chain, per-texture `.meta` settings |
| Scene serialization | JSON `.luth` format, native file dialogs |
| Pipeline cache | VkPipelineCache disk persistence + PipelineManager (lazy, keyed by shader+mode) |
| Skybox / IBL | HDR equirect→cubemap, irradiance convolution, pre-filtered env (5 mips), BRDF LUT, split-sum ambient |

> For descriptor set layout, pass order, and memory budget, see [`arch/rendering-pipeline.md`](arch/rendering-pipeline.md).

---

## Detailed Architecture References

| Area | File | When to read |
|------|------|--------------|
| Fiber system (V1-V6 mitigations) | [`arch/fiber-system.md`](arch/fiber-system.md) | Working on jobs/core |
| Memory (allocators, tracker, STL gap) | [`arch/memory.md`](arch/memory.md) | Working on allocators or memory budgets |
| Profiling (Tracy + GPUTimerPool) | [`arch/profiling.md`](arch/profiling.md) | Adding instrumentation or chasing perf |
| Vulkan validation layers | [`arch/validation-layers.md`](arch/validation-layers.md) | Vulkan setup, debug callback, build-define overrides |
| GPU crash debugging (Aftermath, device-lost) | [`arch/gpu-crash-debugging.md`](arch/gpu-crash-debugging.md) | Chasing `VK_ERROR_DEVICE_LOST` / GPU hangs |
| Version markers (V1-V6) | [`arch/version-glossary.md`](arch/version-glossary.md) | Reading `Vn` source comments — JobSystem hazards vs asset format versions |
| Rendering pipeline (descriptor sets, pass order) | [`arch/rendering-pipeline.md`](arch/rendering-pipeline.md) | Working on renderer |
| Multi-queue (async compute, queue families) | [`arch/multi-queue.md`](arch/multi-queue.md) | Working on async compute / queue submission |
| Frame pipeline (triple-buffer model) | [`arch/frame-pipeline.md`](arch/frame-pipeline.md) | Working on frame pipeline |
| Asset pipeline (importers, loading, caching) | [`arch/asset-pipeline.md`](arch/asset-pipeline.md) | Working on assets/resources |
| Scene & ECS (components, systems, serialization) | [`arch/scene-ecs.md`](arch/scene-ecs.md) | Working on scene/ECS |
| Animation (clips, skeleton, GPU skinning, blending) | [`arch/animation-system.md`](arch/animation-system.md) | Working on animation |
| Physics (Jolt integration, jobified) | [`arch/physics.md`](arch/physics.md) | Working on physics / colliders |
| Editor (panels, IEditorHooks, selection, UI) | [`arch/editor.md`](arch/editor.md) | Working on editor |
