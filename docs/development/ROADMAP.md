# Luth Engine — Roadmap

## Completed Epics

> Summaries are intentionally terse — full writeups in [`history/`](history/), one file per epic slug.

| Version | Epic | Summary | Date |
|---------|------|---------|------|
| v1.0.0 | `job-system` | Fiber-based scheduler: FLS, Chase-Lev work-stealing, MPMC queues, SpinLock, isolated main thread | 2026-03-07 |
| v1.0.0 | `frame-pipeline` | Triple-buffered pipelined execution (Game N / Render N-1 / GPU N-2), MAX_FRAMES_IN_FLIGHT=3 | 2026-03-07 |
| v1.0.0 | `render-graph` | DAG compile with dead-pass culling + batched barriers, serial outer with parallel inner recording | 2026-03-07 |
| v1.0.0 | `cleanup` | Hot-path mutex → SpinLock, deleted GLAD/OpenGL remnants | 2026-03-07 |
| v1.0.0 | `rendering-debug` | Fixed 5 rendering bugs: SceneColor disconnect, depth clear, bindless slot 0, Y-flip, front face winding | 2026-03-15 |
| v1.0.0 | `pbr-material` | Cook-Torrance BRDF, Material SSBO (Set 2), per-RenderMode pipeline variants | 2026-03-15 |
| v1.0.0 | `lighting-shadows` | LightUBO (Set 3), ShadowPass (2048² D32), PCF 3×3, ECS-driven light collection | 2026-03-15 |
| v1.0.0 | `shader-system` | ShaderLibrary singleton, FileWatcher hot-reload, SPIRV-Cross reflection | 2026-03-15 |
| v1.0.0 | `inspector-editor` | Save button + dirty tracking, Add Component dropdown, DirLight shadow controls, albedo color picker | 2026-03-16 |
| v1.0.0 | `shader-asset-pipeline` | ShaderImporter, SPIR-V artifact cache, stable UUIDs via .meta, MaterialSystem dirty sync | 2026-03-19 |
| v1.0.0 | `mipmap-generation` | vkCmdBlitImage chain, TextureSettings pipeline, sampler maxLod | 2026-03-19 |
| v1.0.0 | `scene-serialization` | JSON `.luth` format, Win32 file dialogs, editor File menu + dirty tracking | 2026-03-19 |
| v1.0.0 | `asset-lifetime-fix` | Scene holds shared_ptrs to prevent GC eviction, full async load chain, VMA shutdown fix | 2026-03-21 |
| v1.0.0 | `frame-debugger` | GPUTimerPool, RenderGraphSnapshot, split-panel UI, event slider, named texture registry | 2026-03-22 |
| v1.0.0 | `post-processing` | HDR (RGBA16F), bloom (extract + Gaussian blur), 4 tonemap operators, vignette, grain, CA | 2026-03-22 |
| v1.0.0 | `pipeline-cache` | VkPipelineCache disk persistence, PipelineManager keyed by {shaderUUID, renderMode} with lazy creation | 2026-03-22 |
| v1.0.0 | `skybox-ibl` | HDR equirect→cubemap, irradiance + pre-filtered env (5 mips) + BRDF LUT, split-sum ambient, Set 0 expanded to 4 bindings | 2026-03-23 |
| v1.0.0 | `polish` | Bug fixes (transform/alpha/shadows), Rider theme, inspector overhaul, picking + outline, profiler rework | 2026-03-25 |
| v1.0.0 | `editor-qa` | 22-item QA pass: semantic EditorColors, HDR picker, outline children+occluded, recursive search, primitive geometry, project filters/sort | 2026-03-30 |
| v1.0.0 | `animation-system` | Fiber-parallel sampling, GPU skinning (BoneMatrixBuffer SSBO), SQT blending, crossfade, layered override with masks, root motion | 2026-03-30 |
| v1.0.0 | `smart-import-hot-reload` | Multi-strategy texture discovery, ImportReport/TextureRemapDialog, FileWatcher hot-reload, drop-to-current-dir | 2026-03-31 |
| v1.0.0 | `frame-debugger-upgrade` | Trigger-based capture, per-draw scrubbing, DebuggerState machine, RenderCapturedFrame replay, depth linearization | 2026-04-03 |
| v1.1.0 | `undo-redo` | Command pattern (14 types), UUID-based entity resolution, gizmo drag coalescing, compound commands, material snapshots | 2026-04-09 |
| v1.1.1 | `architecture-cleanup` | RenderingSystem split (4060→2321 LOC): EditorCamera/CameraParams/IBL/FrameDebugger extracted; 9 passes moved to `renderer/passes/` | 2026-04-13 |
| v1.2.0 | `compute-gpu-culling` | Compute pass + buffer support in render graph, GPU frustum cull shader, GPUObjectData SSBO, draws via vkCmdDrawIndexedIndirect | 2026-04-15 |
| v1.3.0 | `csm` | 4-cascade PSSM (bounding-sphere fit), 4-layer shadow array, per-cascade GPU cull, shader cascade selection + blend + bias | 2026-04-16 |
| v1.4.0 | `frame-debugger-sync` | Archive sink + per-pass image staging, frozen-state auto-recapture, hierarchical EventNode tree, per-draw replay | 2026-04-17 |
| v1.5.0 | `gtao` | DepthPrepass + half-res GTAO compute (prefilter → horizon integral → bilateral denoise), Jimenez 2016, Set 0 expanded to 6 | 2026-04-17 |
| v1.6.0 | `arch-cleanup` | Folder reorg: events/ extracted, utils/ dispersed, Components.h split, renderer/ subdivided into 7 concept folders | 2026-04-18 |
| v1.7.0 | `arch-renderer-split` | RenderingSystem 3500→350 LOC: FrameTargets/DrawListBuilder/LightGatherer/CascadeBuilder extracted; RenderPipeline owns graph + resources | 2026-04-18 |
| v2.0.0 | `arch-target-split` | Editor extracted from Luth.lib into Luthien.lib (~12k LOC); IEditorHooks breaks engine→editor include dep; Sandbox.exe descoped | 2026-04-18 |
| v2.1.0 | `shader-asset-pipeline` | Single-stage shader assets (.vert/.frag/.comp = one artifact + UUID); ShaderHeader V2; all 24 engine shaders routed through pipeline | 2026-04-18 |
| v2.2.0 | `math-abstraction` | `Luth::Math` facade — single `<glm/...>` owner. 25 wrappers + templated constants. 37 files migrated, 38 glm includes purged outside facade | 2026-04-18 |
| v2.3.0 | `core-reorg` | `luth/core/` split into `types/` (LuthTypes/LuthMath/TypeTraits), `diagnostics/` (Log/LogFormatters/Profiler), `time/` (Time/Timer) | 2026-04-18 |
| v2.4.0 | `animation-split` | `luth/animation/` dissolved: BoneMatrixBuffer/Skeleton/AnimationClip → `renderer/resources/`; AnimationController → `scene/components/` | 2026-04-19 |
| v2.5.0 | `render-pipeline-split` | `RenderPipeline.cpp` 3104→781 LOC across 7 topic files + new `FrameDebuggerContext`. `CreatePipelines` split into 8 per-family builders | 2026-04-19 |
| v2.6.0 | `rendering-system-slim` | `RenderingSystem` 533→395 LOC: LightingSystem + ShaderWatcher + PickingSystem extracted; cascades pass-by-value | 2026-04-19 |
| v2.7.0 | `editor-cleanup` | Comment audit + Command.h reorg + CommandHistory deque + `Editor::GetPanel<T>` O(1) cache + `Editor::Init` decomposition | 2026-04-19 |
| v2.7.1 | `editor-style-assets` | StylePreset → JSON assets (`luth/assets/styles/*.json`). EditorStyle 616→280 LOC. New `LoadStyle(nameOrPath)`, Save Current As... | 2026-04-19 |
| v2.7.2 | `editor-widgets-reorg` | `luthien/UI.{h,cpp}` split into 5 widget files (Properties/AssetSlot/CollapsingHeader/InfoTable/TexturePreview) under `widgets/` | 2026-04-19 |
| v2.7.3 | `editor-undo-gaps` | All 14 MarkDirty callsites wrapped in commands. New VectorElement/Insert/Erase + EntityActive commands. Drive-by `Entity::isActive` → Disabled tag | 2026-04-23 |
| v2.7.4 | `editor-component-registry` | Hand-written `DrawComponent<T>` switch → type-erased `ComponentDrawerRegistry`. 8 drawers + DebugDrawers consolidated. InspectorPanel 976→255 LOC | 2026-04-23 |
| v2.7.5 | `editor-scene-panel-slim` | `ScenePanel` 1001→443 LOC: ViewportRenderer + GizmoController + ViewportOverlays extracted to `viewport/` | 2026-04-23 |
| v2.8.0 | `play-mode` | Editor state machine (Editing/Playing/Paused) + JSON scene snapshot; AnimationSystem gated; CommandHistory blocks during play; transport bar + viewport tint | 2026-04-23 |
| v2.8.1 | `game-panel` | Dedicated Game panel rendering first Camera entity with letterbox; new `RenderView` + `ViewResources` cache; per-instance resize callback | 2026-04-24 |
| v2.8.2 | `engine-consolidation` | Audit-driven housekeeping: roadmap restructure + 4 new arch docs (memory/profiling/validation-layers/version-glossary) + comment-banner sanitization + Tracy global memory hooks + CPU coverage gaps filled | 2026-04-25 |
| v2.8.3 | `tracy-on-demand` | Hotfix: `TRACY_ON_DEMAND` so Tracy macros no-op when no profiler client connected. Fixes 10 MB/s launcher leak + 0.2 MB/s in-game leak (#30) | 2026-04-25 |
| v2.8.4 | `pipeline-phase-3` | Pipelined CPU stages (Game N / Render N-1) on worker fibers; `RenderSnapshot` POD frozen at game-stage end; two-phase RG dispatch (1 yield/frame); 4 latent JobSystem bugs fixed along the way | 2026-04-26 |
| v2.8.5 | `build-config-foundation` | `luth/core/BuildConfig.h` centralizes detection (`LUTH_BUILD_DEBUG/RELEASE/DIST` + derived flags); engine no longer tests `_DEBUG`/`NDEBUG`; first effort under tag-only release policy | 2026-04-27 |
| v2.8.6 | `frame-debugger-polish` | Unity-style draw-scrub slider + tree-click snap; viewport pass overlay; depth archives via tonemapped preview; archive reuse + 10 Hz throttle eliminating editor freezes under continuous camera movement | 2026-04-28 |
| v2.8.7 | `vulkan-correctness` | Tier-1 hardening: deletion-queue SpinLock (V1), RG WAW barriers, swapchain OUT_OF_DATE + deferred Present rebuild (V2), device Features2 validation, sync2 frame submit + RG-driven present transition | 2026-04-28 |
| v2.8.8 | `animation-quick-pass` | `AnimationClip` becomes UUID-addressable asset (`.anim`); `Animation::ClipUUID` replaces integer indices; drawers swap Combo for `PropertyAsset`; foundation for `animation-controller-v2` | 2026-04-28 |
| v2.8.9 | `persistent-buffer-ring` | Triple-buffered the 3 persistent CPU-mapped SSBOs (ObjectSSBO/IndirectBuffer/Material SSBO) so frame N writes never overlap frame N-1 GPU reads; VMA modernized off deprecated `CPU_TO_GPU` | 2026-04-28 |
| v2.8.10 | `gpu-tagged-heap` | GPU half of Naughty Dog Onion/Garlic: `GPUTaggedPageAllocator` (2 MB pages from host-mapped backings, bulk-free wired to GPU N-2 timeline); Sets 2/4/5 rebind per-frame via UPDATE_AFTER_BIND | 2026-04-29 |
| v2.8.11 | `slot-alloc-spinlock` | Closed v2.8.4 D6 carry-over: `MaterialSystem::m_Lock` + `BoneMatrixBuffer::m_Lock` → `Luth::SpinLock`; arch doc sweep | 2026-04-29 |
| v2.8.12 | `shader-reload-async` | Drops per-save `vkDeviceWaitIdle` from shader hot-reload; defers old pipelines via `PushDeletion` (V1 SpinLock-safe), drained 3 frames later; shader save no longer drops a frame | 2026-04-29 |
| v2.8.13 | `vulkan-polish` | Tier-2/3 cleanup before `jolt-physics`: validation messenger pNext-chained; `BindlessDescriptorSet` LIFO free-list; `RenderResourceCache` keyed on multimap; outline/grid push-constants routed through EditorSettings | 2026-04-29 |
| v2.8.14 | `texture-async-uploads` | Texture half of `vulkan-polish` S4: `UploadImageMipped` records pre-barrier → staging copy → blit chain → `SHADER_READ_ONLY` in one cmd-buffer; 4-slot cmd-buffer ring; deferred-bindless-registration pump | 2026-04-30 |
| v2.9.0 | `editor-foundation` | First AAA editor effort: Gather→Draw lifecycle for panels (parallel gather on worker fibers; main-thread ImGui draws from frozen snapshots); per-panel `LinearAllocator(64K)` gather scratch; 9 panels migrated | 2026-05-01 |
| v2.9.1 | `editor-signal-bus` | Typed `EditorSignal` events on `EventBus` (selection/hierarchy/asset/project/play-state); panels react to subscriptions instead of polling; EventBus hardening (exception-safe dispatch, `SubscriptionHandle`, tracked alloc) | 2026-05-02 |
| v2.9.2 | `editor-console-errors` | New `Log::AddSink` / `ILogSink` interface + `ConsolePanel` (sink + signal-based append, level filter, search, clipper); per-panel error boundary with stack-trace dump via Win32 DbgHelp | 2026-05-02 |
| v2.9.3 | `editor-job-pump` | New `MainThreadPump` static facade — `Post(Callback)` any thread, `Drain()` on main; mirrors v2.9.1 EventBus shape (swap-and-drain, thread-assert, per-callback try/catch); foundation for autosave + thumbnails | 2026-05-02 |
| v2.9.4 | `editor-autosave` | First `MainThreadPump` consumer: periodic side-channel autosave to `<project>/.luth/autosaves/` (never canonical); JSON snapshot on main + file write via IOThread; crash-recovery prompt in OpenScene | 2026-05-03 |
| v2.9.5 | `editor-thumbnails` | ProjectPanel grid switches from FA-icons to rendered previews; `ThumbnailCache` (UUID-keyed, signal-driven invalidation), `ThumbnailGenerator` (worker-fiber CPU bake), `ThumbnailPreviewScene` (Lambert + ambient); disk-persisted | 2026-05-03 |
| v2.9.6 | `editor-undo-fix` | Slider-driven inspector edits stop over-coalescing across release boundaries; new `EditState { changed, committed, itemId }` from every `UI::Property*`; per-T pre-edit value stash between `IsItemActivated`/`IsItemDeactivatedAfterEdit` | 2026-05-04 |
| v2.9.7 | `editor-panels-polish` | ScenePanel toolbar re-laid (gizmo group + transport + render-mode + Debug/Camera/Gizmos splits); HierarchyPanel per-row visibility eye; ProjectPanel grid → BeginTable + clipper; new `EditorSettingsWindow`; `Window` menu + `Reset Layout` | 2026-05-04 |
| v2.9.8 | `editor-inspector-polish` | `EditorClipboard` + `ComponentReset`/`ReplaceCommand` + per-component JSON Copy/Paste in all 8 drawers; new `InspectorHeader` + `Splitter` widgets; **live orbit-cam 3D preview** for Material/Model in pinned footer | 2026-05-05 |
| v2.9.9 | `editor-workspaces` | AAA editor rework closeout: multi-named workspaces (`.ini` + `.workspace.json` sidecar); built-in `Default` ships at `luth/assets/workspaces/`; built-in shadows user copy of same name | 2026-05-05 |
| v2.9.10 | `staging-ring-wrap-overlap` | Hotfix for two boundary-condition bugs in `UploadContext::AllocateStaging` producing intermittent banded pixel mixing under burst loads; explicit `wrapped` tracking + `head == tail` full-ring detection | 2026-05-05 |
| v2.9.11 | `render-hardening` | Audit batch: per-frame UBOs migrate to `GPUTaggedPageAllocator`; `FreeTag(waitValue - 2)` correction (iter T+1 retires before its tag T pages free); Set 3 multi-view race fix; cross-view shadow sync | 2026-05-06 |
| v2.9.12 | `render-pipeline-subsystems` | RenderPipeline god-class extracted into 6 per-domain subsystems (Global/Lighting/Geometry/GTAO/PostProcess/EditorOverlays), each owning lifecycle + descriptor sets + passes; friend coupling fully gone; RP shrinks to ~650 LOC orchestrator | 2026-05-06 |
| v2.9.15 | `frame-debugger-polish-v2` | Batch-handles 3 deferred issues (#98 event-tree group ordering, #99 `Source = Game` replay, #100 Shadow/DepthPrepass/SelectionMask per-draw replay) + 8 drive-by polish; auto-recapture extends to IBL/skybox; cascade label shows splits range | 2026-05-07 |
| v2.10.0 | `jolt-rigid-bodies` | Tier 0 of Jolt: vendor + `LuthJobSystemForJolt` adapter, `Collider`/`RigidBody`/`PhysicsBodyRuntime` components, kinematic/dynamic transform sync, debug-draw subsystem, CCD `motionQuality`; `WaitForCounter` UAF fix along the way | 2026-05-14 |
| v2.10.1 | `jolt-physics-queries` | `Raycast` + `OverlapBox`/`Sphere`/`Capsule`; `LuthContactListener` (Godot-pattern trigger cache under SpinLock); 4-kind event surface + per-frame `DrainEvents` | 2026-05-15 |
| v2.10.2 | `jolt-physics-assets` | `Physics::ShapeCache` (UUID + meshIndex + shapeKind keyed) for `ConvexHullShape` + `MeshShape` from Model data; `PhysicsMaterial` UUID-keyed asset; `ModelImportSettings::PhysicsBakeMode { None, Auto }` opt-in; hot-reload via AssetDatabase callback | 2026-05-15 |
| v2.10.3 | `jolt-character-controller` | Tier 1 `JPH::CharacterVirtual` (requires paired `Collider Type::Capsule`); `ExtendedUpdate` defaults for stair/stick-to-floor; debug-draw colored by `GroundState`; stub `PlayerControllerSystem` until scripting lands | 2026-05-18 |
| v2.11.0 | `custom-fibers` | Custom x86_64 MASM context switch + `VirtualAlloc` stacks replaces Win32 fibers so ASan can track per-fiber stack bounds. TIB ArbitraryUserPointer (`gs:[0x28]`) replaces Win32 FLS. ~5× faster switch (secondary win) | 2026-05-19 |
| v2.11.1 | `foundation-testing` | 28-case stress harness (V1–V6, AtomicCounter, LinearAllocator, TaggedPageAllocator, SpinLock, MPMCQueue, WorkStealingDeque) under DebugASan; caught two engine bugs inline | 2026-05-20 |
| v2.12.0 | `async-compute-queue` | Three-queue Vulkan foundation (graphics + async-compute + transfer) with per-view 3-submit topology, RG queue routing + cross-queue barrier rule (TOP_OF_PIPE substitution on the reader's pre-barrier), CONCURRENT sharing opt-in per cross-queue resource, GTAO chain routed to async compute, UploadContext routed to dedicated transfer queue (DMA engine on discrete GPUs) | 2026-05-20 |
| v3.0.0 | `bindless-migration` | rt-renderer arc opens. BDA enabled (mesh + tagged-heap buffers carry SHADER_DEVICE_ADDRESS_BIT, addresses cached); Set 1 binding 1 sampler array (4 canonical at 0-3, LIFO 4-31); `GPUMaterialData` 4→8 map indices + flag repack; `thumbnail_mesh` migrated to bindless (−120 LOC) | 2026-05-22 |
| v3.0.1 | `slim-gbuffer` | `SlimGBufferPass` writes RG16F oct-normal + R8 roughness + RG16F NDC motion + R16U matID (depth-EQUAL, opaque-only). Adds `prevViewProjection` per-view (multi-view contamination fix), `prevModel` cache, dual-region `BoneMatrixBuffer` for prev-bones. Frame-debugger decoders + 4 `ShadeMode` toggles | 2026-05-22 |
| v3.0.2 | `forward-plus` | Olsson 3D clustered lighting (16×9×24 grid) replaces 64-light UBO. `ClusterBuildPass` + `LightAssignPass` (async-compute) write per-cluster AABBs + atomic-packed light indices; PBR fragment loops cluster's lights (cap 128). Set 3 reshaped to 4 bindings, per-view. New `ShadeMode::ClustersDensity` 3D heat-map | 2026-05-22 |
| v3.0.3 | `volumetric-fog` | Wronski frustum voxel fog (160×90×128 RGBA16F per view). Async-compute inject (cluster lights + CSM + `Component::FogVolume`) + integrate (front-to-back Beer-Lambert); graphics composite via standard alpha-blend. New `VolumetricSubsystem`, `VKTexture` 3D ctor, tagged-union `FogVolume` component + inspector + master toggle | 2026-05-23 |
| v3.0.4 | `volumetric-validation` | Closes v3.0.3 known VUIDs. Image barriers set `VK_QUEUE_FAMILY_IGNORED` (was zero-init for CONCURRENT images); inject/composite declare per-cascade shadow + atlas reads so RG emits DSA → SHADER_READ_ONLY barriers; inject layout gains `UPDATE_AFTER_BIND` for per-frame rewrites. New 4th RG hazard documented | 2026-05-23 |
| v3.0.5 | `volumetric-fog-polish` (initial) | Three deferred sub-tasks (Hillaire multi-scatter, temporal accumulation, ShadeMode viz) plus audit fixes. Temporal moved to dedicated `VolumetricResolve` compute pass after integrate (inject-time blend produced energy non-conservation). HG phase, point-light 1/d², sun-fog absorption, IBL irradiance multi-scatter, half-step transmittance, configurable Quality preset, shader `#include` support, k_ClusterSlicesZ 24→48 | 2026-05-24 |
| v3.0.6 | `volumetric-fog-polish` (follow-up) | Inject split into density + scatter passes joined by RG barrier (enables density-atlas sampling along sun ray — v3.0.5's single-pass `SunFogTransmittance` was dead-code). Canonical σ_t contract drops spurious `× density` in scatter (was double-applying). New `scatteringIntensity` knob, 3D Worley-FBM density noise, recalibrated defaults | 2026-05-25 |
| v3.0.7 | `image-quality` | Closes Phase A. Tokuyoshi19 specular AA + AgX + AgX Punchy tonemaps. Karis14 TAA YCoCg-clip K3 — Halton(2,3) prefix-8 jitter, per-view RGBA16F history ping-pong, ~170 LOC resolve (closest-depth velocity, Blackman-Harris reconstruction, chroma narrow, luma feedback). Roberts R2 blue-noise volumetric dither; pbr.frag → `common/globals.glsl` | 2026-05-25 |
| v3.0.8 | `rt-extensions` | Phase B opener: 4 KHR RT extensions enabled + feature validation + RT-mandatory device check; `VKRayTracingPipeline` + `RtShaderBindingTable` factories; validation-gated no-op `traceRays` smoke test | 2026-05-26 |
| v3.0.9 | `blas-tlas` | Per-mesh BLAS at import (static + skinned, `skinning.comp` deformed-VB + `MODE_UPDATE` refit); per-frame TLAS rebuild on AsyncCompute with FNV-1a hash dirty-skip; Set 0 → 7 bindings (TLAS at b6) | 2026-05-26 |
| v3.0.10 | `rt-shadows` | First RT-pipeline consumer: ray-traced sun shadows (1-spp, R8 mask) as default, raster CSM kept as `ShadowingMode` compare toggle; `RtSunShadowsPass` on AsyncCompute; `pbr.frag` CSM/RT dispatch. Closes Phase B | 2026-05-31 |
| v3.0.10 | `gpu-debug-toolkit` | GPU crash-debugging subsystem: dynamic-load Nsight Aftermath, `LUTH_VALIDATION` runtime tiers (GPU-AV opt-in), per-pass NV checkpoints + device-lost dump; new `arch/gpu-crash-debugging.md` | 2026-05-31 |
| v3.0.11 | `vulkan-sync-hardening` | Clean sync-val baseline before Phase C: GPU object names + RG pass labels, `LUTH_RG_DUMP`/`LUTH_RG_TRACE` + headless solver tests; loadOp-LOAD, swapchain-acquire, cross-queue-WAW barrier fixes | 2026-06-06 |
| v3.0.11 | `gpu-device-lost` | Root-caused the recurring skinned `VK_ERROR_DEVICE_LOST`: 4 MiB `BoneMatrixBuffer` took the tagged-heap large-one-shot destroy path; ND-model fix recycles large tagged allocs (never `vkDestroyBuffer`) | 2026-06-06 |
| v3.0.12 | `restir-di` | Bitterli 2020 ReSTIR DI for point lights: device-local (Garlic) reservoir buffers, rayQuery-in-compute initial RIS + temporal + spatial reuse, demodulated diffuse irradiance remodulated in `pbr.frag`, editor tuning; opens Phase C | 2026-06-07 |
| v3.0.13 | `svgf-denoiser` | Schied 2017 SVGF over the demodulated ReSTIR DI: reproject (temporal EMA + moments) → variance (7×7 spatial fallback) → edge-aware à-trous, behind an `IDenoiser` abstraction; per-view image history + a GENERAL-preserving RG storage read; feedbackTap=−1, A-SVGF deferred | 2026-06-07 |
| v3.0.14 | `restir-gi` | Ouyang 2021 ReSTIR GI: per-pixel 1-bounce path reservoirs + reconnection Jacobian (temporal/spatial reuse, RTXDI BASIC); real bindless secondary-hit material via a per-frame geometry table (`instanceCustomIndex` contract change + buffer_reference deref); second channel-parameterized SVGF instance denoises the bounce; editor tuning | 2026-06-08 |
| v3.0.15 | `gi-polish` | Post-restir-gi follow-ups: TLAS builds for any RT consumer (DI/GI no longer dark under CSM), sun light added to the GI bounce, GI reservoir M·age debug view, and a zero-weight-reservoir fix (unlit hits keep a valid sample point — killed a world-plane confidence split + latent shadowed-region reuse bias) | 2026-06-09 |
| v3.0.16 | `path-trace-reference` | Ground-truth path-traced reference mode: a rayQuery-in-compute megakernel reusing the TLAS + bindless materials, multi-bounce NEE + full Cook-Torrance BRDF + GGX VNDF lobe-MIS, fp32 progressive accumulation reset-on-change, `RenderMode` toggle + editor convergence UI; validates ReSTIR DI/GI convergence | 2026-06-09 |
| v3.0.17 | `rt-reflections` | Stochastic RT specular reflections: GGX-VNDF rays from the slim G-buffer, NEE hit shading via the geometry table, demodulated split-sum composite in `pbr.frag` below a roughness cutoff; dedicated specular denoiser (SvgfDenoiser Reflections channel, hit-distance virtual reprojection); shared `common/brdf.glsl`. Supersedes SSR; opens Phase D | 2026-06-09 |
| v3.0.18 | `volumetric-rt-shadows` | Per-froxel RT shadow rays in the fog inject-scatter pass: point lights + arbitrary occluders now cast fog shadows (sun swaps CSM→RT ray), 1-spp softened by the existing temporal resolve (no denoiser), default-off toggle; TLAS-build hoisted before the volumetric chain for registration-order correctness; folds two post-D.1 ReSTIR-DI audit fixes | 2026-06-09 |
| v3.1.0 | `emissive-parity` | Opens the material-system arc: emissive factor + HDR strength, identical raster (`pbr.frag`) + RT (`geom_table.glsl`) emission closing the raster≠RT bug, editor controls + import bridge + inspector preview + `ShadeMode::Emission`; folds material-authoring fixes — metal/rough/color reach the GPU via direct fields, textureless emissive editable, no autosave reimport bounce | 2026-06-09 |
| v3.1.1 | `material-eval-seam` | Retires `pbr.frag`'s hand-duplicated Cook-Torrance onto the shared `common/brdf.glsl` seam — raster==RT BRDF parity now structural (single source); neutral-renamed the `Pt*` functions; fp32-identical output | 2026-06-10 |
| v3.1.2 | `cutout-rt` | Alpha-cutout materials hole correctly in every RT path: per-instance TLAS opaque flag (cutout→FORCE_NO_OPAQUE) + a shared `geom_table.glsl` candidate-loop alpha-test across PT/GI/reflections/ReSTIR-DI/volumetric fog; rewrites the lone RT-pipeline sun-shadow pass to rayQuery-compute, deleting the SBT/rgen/rmiss | 2026-06-11 |
| v3.1.3 | `transparency-tier` | Transparent tier after the fog composite (fixes #32 skybox compositing): PPLL OIT default (per-view heads image + Garlic node pool, sort-K resolve) + per-view sorted A/B; corrected transparent shading — rayQuery sun shadow, cluster lights, fragment-depth froxel fog; TLAS cull masks make glass shadow-ray-invisible yet GI/reflection/PT-visible | 2026-06-11 |
| v3.1.4 | `rt-normal-maps` | RT hits sample the normal map (TBN at the rayQuery hit, inverse-transpose normal matrix matching `pbr.vert`) + the occlusion map (`HitSurface.ao` → rt-reflections' IBL ambient; PT omits it — geometric occlusion); GI keeps its geometric secondary normal; dead alpha/specular/thickness indices marked reserved | 2026-06-11 |

---

## Planned Epics

Effort scale (scope/difficulty, not calendar time): **S** = small, contained · **M** = some design decisions · **L** = significant refactor or new system · **XL** = full new subsystem.

### Closed series — `rt-renderer` (Mode A, v3.0.0 → v3.0.18) ✅

**CLOSED** — milestone Release "RT Renderer" published; every effort is in the completed table above.

RT-first renderer modernization arc. Clustered Forward+ with bindless throughout, hardware ray tracing for shadows / GI / reflections, full Wronski volumetrics, ReSTIR + SVGF denoising, path-traced reference mode. Target showcase: Bhaal Temple, fully RT-lit. RT-mandatory (raises minimum HW to RT-capable GPU — counted toward the MAJOR bump).

Mode A — series start bumps `Version.h` to `3.0.0` (`bindless-migration`). Intermediate efforts PATCH-bump from there (`v3.0.1` onwards), tag-only — no per-effort Release. Milestone Release at series end.

Umbrella issue: [#127](https://github.com/Hekbas/Luth/issues/127) (sub-effort issues created on demand; commits use `Part of #127` trailer).

**Phase A — Modern foundation**

| Effort | Issue | Size | Notes |
|---|---|---|---|
| A.1 `bindless-migration` ✅ | [#128](https://github.com/Hekbas/Luth/issues/128) | L | BDA enabled + Set 1 sampler array + `GPUMaterialData` 8 maps + `thumbnail_mesh` bindless — shipped v3.0.0 |
| A.2 `slim-gbuffer` ✅ | [#129](https://github.com/Hekbas/Luth/issues/129) | M | Normal RG16F + roughness R8 + motion RG16F + matID R16U; prev-VP/model/bones plumbed — shipped v3.0.1 |
| A.3 `forward-plus` ✅ | [#54](https://github.com/Hekbas/Luth/issues/54) | L | Clustered lighting + light assignment (existing #54 scope) — shipped v3.0.2 |
| A.4 `volumetric-fog` ✅ | [#130](https://github.com/Hekbas/Luth/issues/130) | L | Wronski frustum voxel; light injection + integrate + composite — shipped v3.0.3 (+ `volumetric-fog-polish` v3.0.5/v3.0.6 [#132](https://github.com/Hekbas/Luth/issues/132) added temporal resolve, split inject + canonical math contract, scatter intensity knob, noise modulation) |
| A.5 `image-quality` ✅ | [#135](https://github.com/Hekbas/Luth/issues/135) | M | TAA Karis14 YCoCg-clip recipe + specular AA Tokuyoshi19 + AgX/AgX Punchy tonemaps + blue-noise volumetric dither — shipped v3.0.7. Closes Phase A. |

**Phase B — Hardware RT foundation ✅**

| Effort | Issue | Size | Notes |
|---|---|---|---|
| B.1 `rt-extensions` ✅ | [#137](https://github.com/Hekbas/Luth/issues/137) | M | 4 KHR RT extensions + feature validation + RT pipeline/SBT factories + RT-mandatory device check — shipped v3.0.8 |
| B.2 `blas-tlas` ✅ | [#138](https://github.com/Hekbas/Luth/issues/138) | L | Per-mesh BLAS (static + skinned) + per-frame TLAS rebuild on AsyncCompute + hash dirty-skip — shipped v3.0.9 |
| B.3 `rt-shadows` ✅ | [#140](https://github.com/Hekbas/Luth/issues/140) | L | RT sun shadows default; raster CSM **retained** as `ShadowingMode` compare toggle (not retired — A/B precedent) — shipped v3.0.10. Closes Phase B. |

> Arc-adjacent efforts shipped alongside Phase B (tag-only): `gpu-debug-toolkit` (v3.0.10) — GPU crash-debugging subsystem; `vulkan-sync-hardening` + `gpu-device-lost` (v3.0.11) — clean sync-val baseline + the skinned device-lost root-cause, ahead of Phase C's barrier-heavy work.

**Phase C — RT global illumination**

| Effort | Issue | Size | Notes |
|---|---|---|---|
| C.1 `restir-di` ✅ | [#146](https://github.com/Hekbas/Luth/issues/146) | XL | Bitterli 2020 — device-local reservoirs + rayQuery RIS + temporal/spatial reuse + demodulated DI — shipped v3.0.12 |
| C.2 `svgf-denoiser` ✅ | [#147](https://github.com/Hekbas/Luth/issues/147) | XL | Schied 2017 SVGF + `IDenoiser` abstraction (NRD/RELAX swap reserved); feedbackTap=−1, A-SVGF + feedbackTap=1 deferred to a gated follow-up — shipped v3.0.13 |
| C.3 `restir-gi` ✅ | [#127](https://github.com/Hekbas/Luth/issues/127) | XL | Ouyang 2021 — path reservoirs + reconnection Jacobian + real secondary material (geometry table) + 2nd SVGF instance — shipped v3.0.14 |

**Phase C.5 — Path-traced reference mode**

| Effort | Issue | Size | Notes |
|---|---|---|---|
| C.5 `path-trace-reference` ✅ | [#148](https://github.com/Hekbas/Luth/issues/148) | M | rayQuery megakernel + multi-bounce NEE + full Cook-Torrance BRDF + GGX VNDF lobe-MIS + fp32 progressive accumulation; `RenderMode` toggle — shipped v3.0.16 |

**Phase D — RT reflections + atmospheric polish**

| Effort | Issue | Size | Notes |
|---|---|---|---|
| D.1 `rt-reflections` ✅ | [#149](https://github.com/Hekbas/Luth/issues/149) | L | GGX-VNDF reflections from the slim G-buffer + specular denoiser (hit-distance virtual reprojection) + pbr split-sum composite — shipped v3.0.17 |
| D.2 `volumetric-rt-shadows` ✅ | [#150](https://github.com/Hekbas/Luth/issues/150) | M | Per-froxel RT shadow rays in the inject-scatter pass — **point-light + arbitrary-occluder** fog shadows; sun swaps CSM→RT ray; 1-spp softened by the temporal resolve (no denoiser); default-off toggle — shipped v3.0.18 |

**Out of arc (deferred to follow-up series)**

- `character-shading` — skin (Jimenez15) + hair (Karis16 + Marschner03) + cloth (Estevez17). Triggered by adding a character to Bhaal Temple
- `gpu-driven` — mesh shaders + meshlet baker + HiZ occlusion (Framework 5 alignment)
- `virtual-geometry` — Nanite-class virtualized geometry; long-tail

### Active series — `material-system` (Mode A, v3.1.0)

Extends the uber-shader and factors a shared evaluate-at-surface-point seam that both `pbr.frag` and the RT hit (`geom_table.glsl`) call, retiring the hand-duplicated BRDF. Node-graph deferred to a later authoring layer over the seam. Mode A — series-open MINOR bump to `3.1.0` (`emissive-parity`); intermediate efforts PATCH-bump, tag-only; milestone Release at series end.

Umbrella issue: [#151](https://github.com/Hekbas/Luth/issues/151) (sub-effort issues created on demand; commits use `Part of #151`).

| Effort | Issue | Size | Notes |
|---|---|---|---|
| M.1 `emissive-parity` ✅ | [#152](https://github.com/Hekbas/Luth/issues/152) | S–M | Emissive factor + HDR strength; raster==RT emission; editor/import/preview + `ShadeMode::Emission` — shipped v3.1.0 |
| M.2 `material-eval-seam` ✅ | — | S | `pbr.frag` migrated onto the existing `common/brdf.glsl` seam (`rt-reflections` built it); `Pt*` renamed; raster==RT BRDF parity structural — shipped v3.1.1 |
| M.3 `cutout-rt` ✅ | — | M | Per-instance TLAS opaque flag + shared candidate-loop alpha-test (rayQuery-centric, not anyhit BLAS); cutout correct in RT shadows / GI / reflections / DI / fog; lone RT-pipeline sun-shadow pass rewritten to rayQuery-compute — shipped v3.1.2 |
| M.4 `transparency-tier` ✅ | [#32](https://github.com/Hekbas/Luth/issues/32) | M→L | Dedicated pass after the fog composite: PPLL OIT default + per-view sorted A/B, rayQuery sun shadow + fragment-depth fog, cull-masked RT visibility (shadow-ray-excluded, GI/reflection-visible; scope override: true OIT from the start, superseding "WBOIT only if layered") — shipped v3.1.3 |
| M.5 `rt-normal-maps` ✅ | [#153](https://github.com/Hekbas/Luth/issues/153) | S | RT-hit normal-map TBN + occlusion parity (inverse-transpose normal matrix); applied in PT + rt-reflections, GI keeps its geometric secondary normal; folds the dead-index audit comment — shipped v3.1.4 |

Deferred: `emissive-as-area-lights` (L); a Slang `IMaterial` research spike (M).

> `gpu-particles` ([#57](https://github.com/Hekbas/Luth/issues/57), L) — compute sim, showcase-sized (fire / ember / smoke / motes in god ray). **Parallelizable — not a material-system dependency**; drop into any renderer slot.

### Gameplay enablement

| Pri | Epic | Issue | Target | Effort | Deps |
|---|---|---|---|---|---|
| 4 | `scripting` (C# or Lua) | NEW | v3.2.0 | XL | `rt-renderer` ✅ |
| 5 | `prefab-system` | NEW | v3.2.x | M | `scripting` |

Scripting unblocks the `PlayerControllerSystem` stub deletion and is the prerequisite for most gameplay-side future ideas.

### Animation maturity

| Pri | Epic | Issue | Target | Effort | Deps |
|---|---|---|---|---|---|
| 6 | `animation-controller-v2` | [#94](https://github.com/Hekbas/Luth/issues/94) | v3.2.0 | XL | `animation-quick-pass` ✅ |

### Polish (no fixed slot — opportunistic)

| Epic | Issue | Effort | Notes |
|---|---|---|---|
| `procedural-sky` | NEW | M | Independent; drop into any quiet renderer slot |
| `jiggle-bones` | [#61](https://github.com/Hekbas/Luth/issues/61) | M | Benefits from `jolt-physics` ✅ colliders |
| `rg-aliasing` (optional) | NEW | M | Defer unless `rt-renderer` Phase A/D pressures transient VRAM |

> `fxaa-taa` ([#72](https://github.com/Hekbas/Luth/issues/72)) — TAA absorbed into rt-renderer Phase A.5; FXAA dropped (TAA is strictly better given motion vectors land in Phase A.2).

> Detailed design lives in `docs/development/epics/<slug>.md` (local, never committed) once an epic enters plan-mode. The arch docs ([`arch/`](arch/)) are the canonical reference for system invariants.

---

## Versioning

Semantic Versioning (`MAJOR.MINOR.PATCH`):
- **MAJOR** — fundamental architecture changes or engine rewrites
- **MINOR** — each completed epic with user-visible changes
- **PATCH** — bug fixes and polish between epics

Version is centralized in `luth/source/luth/core/Version.h`.

---

## Future Ideas

Long-tail wishlist of unscheduled work lives in [`FUTURE.md`](FUTURE.md). Categories: physics maturity, gameplay enablement, rendering (beyond planned-epic deps), animation maturity, audio, editor & tools, profiling / memory.
