# Luth Engine — Roadmap

## Completed Epics

| Version | Epic | Summary | Date |
|---------|------|---------|------|
| v1.0.0 | `job-system` | Fiber-based scheduler: FLS, Chase-Lev work-stealing, MPMC queues, SpinLock, isolated main thread | 2026-03-07 |
| v1.0.0 | `frame-pipeline` | Triple-buffered pipelined execution (Game N / Render N-1 / GPU N-2), unified MAX_FRAMES_IN_FLIGHT=3 | 2026-03-07 |
| v1.0.0 | `render-graph` | DAG compile with dead-pass culling + batched barriers, serial execution with parallel inner recording | 2026-03-07 |
| v1.0.0 | `cleanup` | Hot-path mutex → SpinLock, deleted GLAD/OpenGL remnants, removed temp files | 2026-03-07 |
| v1.0.0 | `rendering-debug` | Fixed 5 rendering bugs: SceneColor disconnect, depth clear, bindless slot 0, Y-flip, front face winding | 2026-03-15 |
| v1.0.0 | `pbr-material` | Cook-Torrance BRDF, Material SSBO (Set 2), per-RenderMode pipeline variants | 2026-03-15 |
| v1.0.0 | `lighting-shadows` | LightUBO (Set 3), ShadowPass (2048² D32), PCF 3×3, ECS-driven light collection | 2026-03-15 |
| v1.0.0 | `shader-system` | ShaderLibrary singleton, hot-reload via FileWatcher, SPIRV-Cross reflection enabled | 2026-03-15 |
| v1.0.0 | `inspector-editor` | Save button, dirty tracking, Add Component (MeshRenderer/Animation), DirLight shadow controls, albedo color picker | 2026-03-16 |
| v1.0.0 | `shader-asset-pipeline` | ShaderImporter, SPIR-V artifact cache, stable UUIDs via .meta, fixed AssetDatabase init bugs, MaterialSystem dirty sync | 2026-03-19 |
| v1.0.0 | `mipmap-generation` | vkCmdBlitImage chain, TextureSettings pipeline (.meta → importer → artifact → VKTexture), sampler maxLod | 2026-03-19 |
| v1.0.0 | `scene-serialization` | JSON `.luth` format, Win32 file dialogs, editor File menu + shortcuts, dirty tracking, ProjectPanel scene loading | 2026-03-19 |
| v1.0.0 | `asset-lifetime-fix` | Scene holds shared_ptrs to prevent GC eviction, MaterialSystem shared_ptr, full async load chain, VMA shutdown fix | 2026-03-21 |
| v1.0.0 | `frame-debugger` | GPUTimerPool, RenderGraphSnapshot, split-panel UI, event slider, named texture registry | 2026-03-22 |
| v1.0.0 | `post-processing` | HDR pipeline (RGBA16F), bloom (extract + Gaussian blur), tonemapping (4 operators), vignette, grain, CA | 2026-03-22 |
| v1.0.0 | `pipeline-cache` | VkPipelineCache disk persistence (cache/pipeline.bin), PipelineManager keyed by {shaderUUID, renderMode} with lazy creation and targeted hot-reload invalidation | 2026-03-22 |
| v1.0.0 | `skybox-ibl` | HDR equirect→cubemap, irradiance convolution, pre-filtered env map (5 mips), BRDF LUT, PBR split-sum ambient, skybox pass (depth=1.0 trick), Set 0 expanded to 4 bindings | 2026-03-23 |
| v1.0.0 | `polish` | Bug fixes (transform dispatch, alpha cutoff, shadows), Rider theme, inspector overhaul, editor persistence/layouts, mouse picking + selection outline + shade modes, profiler rework | 2026-03-25 |
| v1.0.0 | `editor-qa` | 22-item QA pass: semantic EditorColors, skybox HDR picker, outline children+occluded-fade, recursive hierarchy search, Point Light/Camera context menu entries, primitive geometry creation, project panel folder search, eager scene dep loading, resource panel Font/Scene filters + type icons + column sorting + table polish | 2026-03-30 |
| v1.0.0 | `animation-system` | Fiber-parallel keyframe sampling, GPU skinning (BoneMatrixBuffer SSBO), SQT blending, crossfade transitions, layered override with bone masks, root motion extraction, bone debug overlay, full editor inspector integration | 2026-03-30 |
| v1.0.0 | `smart-import-hot-reload` | Multi-strategy texture discovery (4 search strategies), ImportReport + TextureRemapDialog, Project Panel hot reload via FileWatcher (1 s polling, Created/Modified/Deleted), drop-to-current-dir, eager texture copy + import on drop, TOCTOU crash fix in FileWatcher, Assets menu "Resolve Missing Textures..." | 2026-03-31 |
| v1.0.0 | `frame-debugger-upgrade` | Trigger-based capture, per-draw-call scrubbing, DebuggerState machine (Inactive→CaptureRequested→Frozen), full pass instrumentation, RenderCapturedFrame re-recording, rescue blit for truncated frames, depth linearization shader, rewritten panel UI | 2026-04-03 |
| v1.1.0 | `undo-redo` | Command pattern with 14 command types, UUID-based entity resolution, gizmo drag coalescing, compound commands, material snapshot undo, HistoryPanel debug UI, 3 stale-handle crash fixes | 2026-04-09 |
| v1.1.1 | `architecture-cleanup` | RenderingSystem split (4,060→2,321 LOC): EditorCamera extraction, CameraParams decoupling, IBLPrecompute, FrameDebugger class, 9 render passes to `renderer/passes/`, Command.h modularized into `editor/commands/` (6 sub-headers) | 2026-04-13 |
| v1.2.0 | `compute-gpu-culling` | Render graph compute pass + buffer support, `VulkanComputePipeline`, GPU frustum cull compute shader, GPUObjectData SSBO (Set 5), all draw passes converted to `vkCmdDrawIndexedIndirect`, IBLPrecompute refactored, Frame Debugger extended with indirect + compute capture | 2026-04-15 |
| v1.3.0 | `csm` | 4-cascade PSSM (Sascha Willems bounding-sphere fit), 4-layer shadow array, per-cascade GPU cull, cascade selection + blending + bias in PBR shader, cascade debug viz | 2026-04-16 |
| v1.4.0 | `frame-debugger-sync` | Archive sink + per-pass image staging, frozen-state model with auto-recapture on camera move, hierarchical EventNode tree (Group/Pass/Cascade/Draw), per-draw replay-then-copy for GeometryPass, CSM cascade detail panel + linearized depth preview, deferred archive teardown | 2026-04-17 |
| v1.5.0 | `gtao` | DepthPrepass + half-res GTAO compute chain (prefilter → horizon integral → bilateral denoise), Jimenez 2016 analytical slice integral with VS-normal reconstruction from depth derivatives, Set 0 expanded to 6 bindings, editor UI + visualize mode, R8/R32F formats added across renderer/RG/FrameDebugger | 2026-04-17 |
| v1.6.0 | `arch-cleanup` | events/ extracted from platform/; utils/ dispersed to editor/core/resources; FrameData moved to core/; Systems→SystemRegistry with unique_ptr ownership fix; Components.h split into components/ subfolder (umbrella preserved); POD component members m_X→Value; renderer/ subdivided into 7 concept folders (resources/material/shader/pipeline/lighting/settings/draw); LightTypes.h extracted from RenderingSystem | 2026-04-18 |
| v1.7.0 | `arch-renderer-split` | RenderingSystem god-class dissolved (3 500→350 LOC, −90%): FrameTargets, DrawListBuilder, LightGatherer, CascadeBuilder extracted; RenderPipeline owns graph assembly + all graphics resources (pipelines, descriptor sets, SPIR-V, UBOs/SSBOs, IBL maps, bloom textures, GPU timers, preview textures, named-texture registry); RS retains ECS-glue state only (CameraParams, ShadowParams, Cascades, FrameTargets, FrameDebugger, editor toggles); animation/ module consolidates AnimationClip + Skeleton + BoneMatrixBuffer + AnimationController | 2026-04-18 |
| v2.0.0 | `arch-target-split` | Editor extracted from Luth.lib into new Luthien.lib (~12 k LOC moved to luthien/source/luthien/); luthien/ exe folder renamed to runtime/; IEditorHooks interface breaks engine→editor include dependency (App/Input/Luth.h route through nullptr-safe hook registry populated by LuthienApp::CreateApp); EditorViewportState snapshot replaces per-getter dispatch for camera/IBL/selection; Sandbox.exe descoped (structural guarantee enforced by `git grep luth/source luthien/\|Luthien` = zero); new lepch.h editor PCH; VS project-name split (Runtime project, targetname Luthien) | 2026-04-18 |
| v2.1.0 | `shader-asset-pipeline` | Single-stage shader assets (.vert/.frag/.comp each one artifact + UUID); `ShaderImporter` rewritten to compile one file per artifact (no .vert+.frag pairing); `ShaderHeader` schema V2 rejects V1 artifacts; `Shader`/`VulkanShader` hold one stage + one module; `ShaderLibrary::LoadEngine` idempotent loader + registrar; all 24 engine shaders routed through the asset pipeline (no runtime `ShaderCompiler::Compile` fallback in RenderPipeline / IBLPrecompute); hot-reload fires on any stage (incl .comp) with per-shader pipeline rebuild; `RecompileUtilityShaders` fallback deleted; startup `Fragment shader not found` error eliminated | 2026-04-18 |
| v2.2.0 | `math-abstraction` | `Luth::Math` facade — single owner of `<glm/...>` includes (Math.h + LuthTypes.h only). Templated constants (`Math::Pi<T>`/`TwoPi<T>`/`HalfPi<T>`/`SmallNumber<T>`/`KindaSmallNumber<T>`/`FloatMax<T>`/`FloatLowest<T>`/...) delegating to `<numbers>` and `<limits>` where applicable. 25 function wrappers (`Math::Translate/Rotate/Scale/Perspective/Ortho/LookAt/Inverse/Transpose/Normalize/Length/Length2/Dot/Cross/Mix/Slerp/ToMat4/EulerAngles/QuatLookAt/Decompose/Radians/Degrees/Clamp/Min/Max/Abs`) plus `ValuePtr/MakeVec3` pointer helpers and `Math::length_t/qualifier` re-exports. Alias set extended (`IVec2/3/4`, `UVec2/3/4`, `Mat2`). LuthTypes.h dropped 6 unused magic constants (`PI/EPSILON/FLOAT_MAX/...`). Bulk-rewrite migrated 37 files (450 `glm::` refs collapsed to 50 inside the facade only); 38 `<glm/...>` includes purged outside the facade. Latent `numeric_limits::min` vs `lowest` confusion in AABB sentinel resolved | 2026-04-18 |
| v2.3.0 | `core-reorg` | `luth/core/` reorganized into three semantic sub-folders. `types/` houses `LuthTypes.h` (primitives only — i8…u64, f32/f64, byte, fs alias), `LuthMath.h` (Vec/IVec/UVec/Mat/Quat aliases + sizeof asserts + the full `Math::` facade + Assimp/AABB/Frustum helpers — formerly `Math.h`), and `TypeTraits.h` (`IsGLMVector`/`IsGLMMatrix`). `diagnostics/` houses `Log.{h,cpp}`, `LogFormatters.h` (now also owns the ostream<< Vec3/Mat4 operators previously in `LuthTypes.h`), and `Profiler.h`. `time/` houses `Time.h` and `Timer.h`. Top-level lifecycle (App, EntryPoint, Version, FrameData, UUID, EditorHooks, ProjectFile) stays at `core/` root. Dead `Luth::Normalize`/`Cross` forward decls deleted (no definitions, no callers — superseded by `Math::*`). 132 caller files rewritten; LuthTypes.h consumers split by Vec/Mat/Quat usage (18 → LuthMath.h; 48 → LuthTypes.h primitives-only) | 2026-04-18 |
| v2.4.0 | `animation-split` | `luth/animation/` dissolved — each file lands with its actual owner. `BoneMatrixBuffer.{h,cpp}` + `Skeleton.h` + `AnimationClip.h` → `renderer/resources/` (Skeleton/AnimationClip are asset data already held by-value inside `Model.h`; BoneMatrixBuffer is a Vulkan SSBO singleton). `AnimationController.h` → `scene/components/`, aligning path with its existing `namespace Luth::Component` declaration. 20 include sites rewritten via perl; 3 atomic refactor commits, each builds Debug x64 clean | 2026-04-19 |
| v2.5.0 | `render-pipeline-split` | `RenderPipeline.cpp` god-orchestrator split across 7 topic files + new `FrameDebuggerContext` class (3,104 → 781 LOC, −75%). `InitShadowResources` + `InitIBLResources` + `ReloadSkybox` → `renderer/lighting/`; `InitPostProcessResources` + `UpdatePostProcessDescriptors` + `UpdatePostProcessUBO` → new `renderer/postprocess/`; `InitAOResources` + `UpdateAODescriptors` + `UpdateGTAOUBO` → `renderer/passes/`; `InitGlobalUniforms` + `UpdateGlobalUniforms` → `renderer/resources/`; `InitObjectSSBODescriptorLayout` + `InitGPUObjectBuffers` + `InitCullPipeline` + `BuildGPUObjectBuffer` + `EnsureMaterialRegistered` + `UploadLightUBO` → new `renderer/gpu/`. `CreatePipelines` broken into 8 per-family builders (`BuildPBRPipelines`/`BuildShadowPipelines`/`BuildDepthPrepassPipelines`/`BuildSelectionPipelines`/`BuildSkyboxPipeline`/`BuildPostPipelines`/`BuildOutlinePipeline`/`BuildGridPipeline`) in `renderer/pipeline/PipelineFactory.cpp`. Preview textures + 8 debugger methods extracted into `FrameDebuggerContext` class in new `renderer/debug/`; `RenderPipeline` holds `unique_ptr<FrameDebuggerContext>` + forwarding accessors so external callers stay unchanged. 7 atomic refactor commits, each builds Debug x64 clean | 2026-04-19 |
| v2.6.0 | `rendering-system-slim` | `RenderingSystem` shed three responsibilities (533 → 395 LOC, 37 → 14 `.cpp` includes). New `scene/systems/LightingSystem` owns `LightGatherer` + `CascadeBuilder` + `LightUniforms` + `CascadeData` + `DirectionalLightShadowParams`; `RenderPipeline::UpdateGlobalUniforms` takes cascades + shadow params by value, replacing the `m_System.m_Cascades/m_ShadowParams` friend reads — one leg of the `friend class RenderPipeline` coupling gone. New `renderer/shader/ShaderWatcher` service wraps `FileWatcher` + reload queue; `RenderPipeline` owns the instance, `Initialize`/`Shutdown` manage lifetime, `Execute` prologue calls `Poll()`. New `scene/systems/PickingSystem` owns the 59-line Vulkan readback + pending-pick state; registered after `RenderingSystem` so `Update` runs post-render with a valid EntityID buffer; editor panels reach it via `SystemRegistry::GetSystem<PickingSystem>()`. `RenderingSystem` now a narrow ECS→DrawList→Pipeline dispatcher; all three extractions build Debug x64 clean in atomic commits | 2026-04-19 |

> Detailed writeups in [`history/`](history/) — `v1.x/` and `v2.x/` subfolders, one file per epic slug.

---

## Planned Epics

A post-v2.0 architecture-review series is in progress; feature epics resume after it.
Full roadmap + per-epic scope: [`../../plans/analyze-my-engine-in-magical-moore.md`](https://github.com/Hekbas/Luth) (local plan file).

| Priority | Epic | Issue | Target | Est. Time | Deps |
|----------|------|-------|--------|-----------|------|
| 1 | `play-mode` | [#66](https://github.com/Hekbas/Luth/issues/66) | v2.7.0 | 1-2 weeks | — |
| 2 | `jolt-physics` | [#56](https://github.com/Hekbas/Luth/issues/56) | v2.8.0 | 2-3 weeks | `play-mode` |
| 3 | `jiggle-bones` | [#61](https://github.com/Hekbas/Luth/issues/61) | v2.8.1 | 1 week | — |
| 4 | `forward-plus` | [#54](https://github.com/Hekbas/Luth/issues/54) | v2.9.0 | 2 weeks | `compute-gpu-culling` |
| 5 | `fxaa-taa` | [#72](https://github.com/Hekbas/Luth/issues/72) | v2.9.1 | 1 week | — |
| 6 | `gpu-particles` | [#57](https://github.com/Hekbas/Luth/issues/57) | v2.10.0 | 2-3 weeks | `compute-gpu-culling`, `forward-plus` |

> Full specs and dependency graph: [`BACKLOG.md`](BACKLOG.md)

---

## Versioning

Semantic Versioning (`MAJOR.MINOR.PATCH`):
- **MAJOR** — fundamental architecture changes or engine rewrites
- **MINOR** — each completed epic with user-visible changes
- **PATCH** — bug fixes and polish between epics

Version is centralized in `luth/source/luth/core/Version.h`.

---

## Future Ideas (Not Scoped)

> Items covered by Planned Epics above are tracked there. Below are ideas beyond the current backlog.

### Rendering
- Deferred GBuffer rendering
- Global illumination (screen-space or probe-based)
- Volumetric fog / haze
- SSR (screen-space reflections)
- HZB occlusion culling

### Gameplay Systems
- Animation v2: DQS, morph targets, IK, state machines, animation LODs
- Prefab system (reusable entity templates)
- Scripting (C# via Mono, or Lua)

### Audio
- 3D spatial audio
- Audio asset pipeline

### Editor & Tools
- Asset streaming (async GPU upload pipeline)
- Visual shader editor
