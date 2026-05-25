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
| v3.0.0 | `bindless-migration` | rt-renderer arc opens. `bufferDeviceAddress` enabled (feature + VMA bit + `SHADER_DEVICE_ADDRESS_BIT` on mesh + tagged-heap buffers; addresses cached on `VKVertex`/`VKIndexBuffer`). Set 1 binding 1 = `VK_DESCRIPTOR_TYPE_SAMPLER` array (32 slots, 4 canonical samplers at fixed 0-3 + `BindSampler`/`UnbindSampler` LIFO over 4-31). `GPUMaterialData` extended to 8 map indices + flag repack (UV 8-15 → 16-23, new HAS_* bits 5-7). `thumbnail_mesh` migrated to bindless sampling (net −120 LOC). All compose with existing primitives — no new allocator/sync/descriptor set | 2026-05-22 |
| v3.0.1 | `slim-gbuffer` | rt-renderer A.2. New `SlimGBufferPass` after `DepthPrepass` writes RG16F oct-normal + R8 roughness + RG16F NDC motion + R16U matID at viewport res; depth-EQUAL against prepass, opaque-only. Three pieces of new state: `prevViewProjection` in `GlobalUniforms` (per-view storage on `ViewResources`, not global — fixed mid-effort after multi-view contamination surfaced), `prevModel` in `GPUObjectData` (render-side `m_PrevModelByEntity` cache), dual-region `BoneMatrixBuffer` for prev-frame bones (sibling 2 MB scratch + 2× GPU region per frame; `prevBoneOffset = boneOffset + 32768` reuses former `_pad` slot). Frame-debugger decoders (`debugSlimDecode` / `debugSlimMatID`) + 4 `ShadeMode` toggles (live `SlimVizPass` blits selected attachment to LDR). `R16_Uint` + `RG16_Float` formats threaded through `Texture` + `RG::TextureFormat` enums. No new allocator/sync/descriptor set; no Set 0/1/4/5 reshape. Cutout coverage deferred (cutouts fail depth-EQUAL — prepass clears to 1.0) | 2026-05-22 |
| v3.0.2 | `forward-plus` | rt-renderer A.3. Olsson 3D clustered lighting replaces the fixed 64-light `LightUBO`. View frustum split into `16 × 9 × 24 = 3456` clusters; `ClusterBuildPass` (async-compute, `cluster_build.comp`) writes per-cluster view-space AABBs via log depth slicing; `LightAssignPass` (async-compute, `light_assign.comp`) sphere-vs-AABB per cluster + `atomicAdd`-packs per-cluster light indices. PBR fragment derives cluster ID from `gl_FragCoord` + Olsson-linearized depth and loops only the cluster's lights. Per-cluster cap raised from 64 to `k_MaxLightsPerCluster = 128`. Set 3 reshaped 2 → 4 bindings (LightSSBO std430 header+flexible array, ClusterGrid uvec2 SSBO, LightIndex uint SSBO, shadow sampler at b3) and moved to per-view `ViewResources::lightDescSet` (cluster grid + index differ between Scene + Game panel views). `LightUniforms` deleted; `LightGatherer` + `CaptureSnapshot` 64-cap dropped. `ShadeMode::ClustersDensity` true 3D depth-sampled heat-map (`cluster_viz.frag` samples SceneDepth, computes per-fragment Olsson slice, tints by count). All composes with `GPUTaggedPageAllocator` + per-frame-descriptor-set-cycling + async-compute queue topology — no new allocator/sync primitive. Two bugs caught in smoke (BufferHandle 1-based indexing; lost SubRegion offsets in cluster bindings) → new RG hazard documented (BufferHandle is for barrier tracking, not descriptor binding) | 2026-05-22 |
| v3.0.3 | `volumetric-fog` | rt-renderer A.4. Wronski frustum voxel volumetric fog. Per-view 160×90×128 RGBA16F 3D atlas; two async-compute passes (`VolumetricInjectPass` writes per-voxel dir-light + cluster point lights + CSM shadow attenuation + local `Component::FogVolume` modulation; `VolumetricIntegratePass` walks Z front-to-back accumulating Beer-Lambert transmittance + in-scatter). Graphics composite pass blends the integrated atlas into HDR sceneColor via standard alpha blend — shader emits `(fogColor, fogOpacity)` so `src·src.a + dst·(1-src.a)` produces the desired Beer-Lambert composite without feedback loops or scratch HDR targets. Analytic global distance + height fog layered in composite. `VKTexture` gains generalized 3D ctor (`VK_IMAGE_TYPE_3D` + `VK_IMAGE_VIEW_TYPE_3D` branching, null sampler). New `VolumetricSubsystem` sibling to GTAOSubsystem; new `Component::FogVolume` tagged-union (Box / Sphere) with inspector drawer; `EditorSettings.enableVolumetricFog` master toggle gates the chain. Persistent atlases via VMA, per-frame FogVolume SSBO via `GPUTaggedPageAllocator`. Temporal accumulation + Hillaire multi-scatter + ShadeMode debug viz deferred to follow-up `volumetric-fog-polish` (atlas slot allocated, hooks present). Two benign validation issues documented (sceneDepth DSA-vs-SHADER_READ across queue cycle; inject UAB on per-frame rewrites) — non-blocking, owned by follow-up | 2026-05-23 |
| v3.0.4 | `volumetric-validation` | Closes the v3.0.3 known-issue VUIDs. Originally scoped as `rg-depth-handoff` after the v3.0.3 history misdiagnosed VUID-09600 as a sceneDepth cross-queue layout-handoff bug; targeted instrumentation revealed the offending VkImage was actually the shadow map (`arrayLayer=3`), not sceneDepth. Four fixes: (1) `RenderGraph::Execute` image barriers now set `srcQueueFamilyIndex = dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED` per `VUID-VkImageMemoryBarrier2-image-04071` for CONCURRENT-shared images — buffer barriers + 30+ other sites already did this, central RG was the outlier. (2) `AddInjectPass` takes `shadowHandles[]` and declares per-cascade `ReadStorageImage` so the solver emits `DSA → SHADER_READ_ONLY` barriers (silences VUID-09600 on shadow array). (3) `AddIntegratePass` reuses inject's atlas handles (no double `ImportResource` — arch hazard #1) and returns post-write `inScatter`; `AddCompositePass` takes it and declares `builder.Read` (silences VUID-09600 on volInScatter). (4) Inject descriptor layout bindings 2-5 get `UPDATE_AFTER_BIND` flag + layout `UPDATE_AFTER_BIND_POOL` flag (silences VUID-03047 on per-frame rewrites). New 4th hazard entry in `arch/rendering-pipeline.md`. Follow-up noted: `isCrossQueue` only tracks `lastWriter`, missing the read-then-write-cross-queue case — harmless under semaphore-driven sync, picked up in a future RG hardening pass | 2026-05-23 |
| v3.0.5 | `volumetric-fog-polish` (initial) | rt-renderer A.4 follow-up. Lands the three deferred sub-tasks from `volumetric-fog` (Hillaire multi-scatter scalar, temporal accumulation, ShadeMode debug viz) plus 9 audit-driven fixes. Load-bearing change: **temporal accumulation moved to a dedicated `VolumetricResolve` compute pass** after integrate (the initial inject-time blend produced energy non-conservation, ~15% of correct brightness under stationary camera). Resolve reads scratch + reprojected prev-resolved + Karis 3×3×3 clamp + temporalAlpha blend → parity-ping-pong over `volInScatterHistA/B`. Plus: HG phase function (g configurable), point-light 1/d², sun-fog 4-step absorption ray-march, IBL irradiance for proper Wronski multi-scatter (replaces misnamed multiplicative boost), emit-term physics fix, half-step transmittance for slice-center alignment, composite invView push constant, sky-fog opacity cap, configurable atlas resolution (Quality enum Low/Medium/High), shader `#include` support via shaderc IncluderInterface, all 11 settings exposed in RenderPanel + tooltips, FogVolume viewport gizmos, tone-mapped viz output, k_ClusterSlicesZ 24→48 (halves Z-banding granularity) | 2026-05-24 |
| v3.0.6 | `volumetric-fog-polish` (follow-up) | rt-renderer A.4 follow-up², driven by post-v3.0.5 smoke testing. **Inject pass split into two compute passes** (density + scatter) joined by RG barrier — unlocks sampling the density atlas at neighboring voxels for proper Hillaire-style sun-ray absorption (the v3.0.5 single-pass `SunFogTransmittance` was dead-code: `steps` cancelled out of the math, producing the same `exp(-0.5·farZ·voxelDensity)` regardless of step count). Density pass writes `vec4(density, tint.rgb)` to volDensity (tint packed into `.gba`); scatter pass reads via sampler3D for CSM+HG+multi-scatter math + along-the-sun-ray density samples. **Canonical inject/integrate math contract**: drops spurious `× density` in scatter — integrate's `(1 − exp(−σ_t · dt))` already supplies the σ_t path factor (Wronski 2014 / Hillaire 2015); pre-multiplying double-applied σ_t and dimmed fog by ~10× at density 0.1. CONTRACT comment in both shaders + new "Cross-pass numerical contracts" hazard in `arch/rendering-pipeline.md`. New `scatteringIntensity` post-canonical artistic multiplier (UE5 / Frostbite-style knob) for the off-axis brightness lift HG inherently leaves dim. New 3D Worley-FBM density-modulation noise (128³ bake at Init, world-space sample with wind drift). Default settings recalibrated for canonical math (density 0.1, anisotropy 0.7, multi-scatter 0.15, sun-steps 2, scatteringIntensity 15, Quality High). Three intervening fixes: shader compile errors on first scene load, cluster_build/light_assign SLICES_Z mismatch, viz desc-pool silent overflow | 2026-05-25 |
| v3.0.7 | `image-quality` | rt-renderer A.5 — closes Phase A. **Tokuyoshi19 specular AA** (screen-space normal-curvature variance lifted into BRDF roughness in pbr.frag; default-on at sigma 0.5; pbr.frag migrated from inlined UBO to `common/globals.glsl` — v3.0.5 deferred work). **AgX + AgX Punchy tonemaps** (Wrensch fitted polynomial, Three.js post-r161 lineage, EaryChow-reviewed; CONTRACT comment that AgX returns linear sRGB — tail gamma stays). **Karis14 TAA YCoCg-clip recipe** — Halton(2,3) prefix-8 jitter, per-view RGBA16F history ping-pong (taaHistoryA/B parity), full L2 resolve shader (~170 LOC): closest-depth velocity dilation + 9-tap YCoCg neighborhood + rounded box+plus AABB + chroma narrow + clip_aabb toward center + Blackman-Harris 3.3 reconstruction + luma-distance feedback + off-screen UV rejection. clip_aabb + RGB_YCoCg lifted from [playdead/temporal](https://github.com/playdeadgames/temporal) (MIT). HDR-domain TAA inserted between volumetric composite and bloom; grid pass writes on TAA output. **Blue-noise volumetric dither** (Roberts R2 plastic-number quasi-random sequence, one-line bake, 64² R8, NEAREST+REPEAT; jitters per-fragment atlas slice ±0.5 to break Wronski log-slice Z-banding — TAA integrates over ~6 frames into smooth gradients). RenderPanel "Anti-Aliasing" header + tooltips. Per-view jitter state on ViewResources avoids the multi-view contamination hazard. Pool capacity bumped (sets 64→96, samplers 96→128, UBOs 32→48). +64 MB per scene+game view for TAA history. K3 (YCoCg AABB clip) chosen over K4 (Salvi variance-clip) because production-tested pairing with N=8 Halton, K3→K4 follow-up is ~25 LOC and zero architectural commitment. | 2026-05-25 |

---

## Planned Epics

Effort scale (scope/difficulty, not calendar time): **S** = small, contained · **M** = some design decisions · **L** = significant refactor or new system · **XL** = full new subsystem.

### Active series — `rt-renderer` (Mode A, v3.0.0)

RT-first renderer modernization arc. Clustered Forward+ with bindless throughout, hardware ray tracing for shadows / GI / reflections, full Wronski volumetrics, ReSTIR + SVGF denoising, path-traced reference mode. Target showcase: Bhaal Temple, fully RT-lit. RT-mandatory (raises minimum HW to RT-capable GPU — counted toward the MAJOR bump).

Mode A — series start bumps `Version.h` to `3.0.0` (`bindless-migration`). Intermediate efforts PATCH-bump from there (`v3.0.1` onwards), tag-only — no per-effort Release. Milestone Release at series end.

Umbrella issue: [#TBD] (sub-effort issues created on demand; commits use `Part of #<umbrella>` trailer).

**Phase A — Modern foundation**

| Effort | Issue | Size | Notes |
|---|---|---|---|
| A.1 `bindless-migration` | NEW | L | Descriptor indexing across all shaders; foundational for cluster light lookups + RT material handles |
| A.2 `slim-gbuffer` | NEW | M | Normal + roughness + motion vectors + material ID; feeds TAA + RT denoising |
| A.3 `forward-plus` ✅ | [#54](https://github.com/Hekbas/Luth/issues/54) | L | Clustered lighting + light assignment (existing #54 scope) — shipped v3.0.2 |
| A.4 `volumetric-fog` ✅ | [#130](https://github.com/Hekbas/Luth/issues/130) | L | Wronski frustum voxel; light injection + integrate + composite — shipped v3.0.3 (+ `volumetric-fog-polish` v3.0.5/v3.0.6 [#132](https://github.com/Hekbas/Luth/issues/132) added temporal resolve, split inject + canonical math contract, scatter intensity knob, noise modulation) |
| A.5 `image-quality` ✅ | [#135](https://github.com/Hekbas/Luth/issues/135) | M | TAA Karis14 YCoCg-clip recipe + specular AA Tokuyoshi19 + AgX/AgX Punchy tonemaps + blue-noise volumetric dither — shipped v3.0.7. Closes Phase A. |

**Phase B — Hardware RT foundation**

| Effort | Issue | Size | Notes |
|---|---|---|---|
| B.1 `rt-extensions` | NEW | M | `VK_KHR_acceleration_structure` + `ray_tracing_pipeline` integration |
| B.2 `blas-tlas` | NEW | L | Per-mesh BLAS + per-frame TLAS rebuild for dynamic objects |
| B.3 `rt-shadows` | NEW | L | Replace raster CSM with RT shadow rays (CSM path retired) |

**Phase C — RT global illumination**

| Effort | Issue | Size | Notes |
|---|---|---|---|
| C.1 `restir-di` | NEW | XL | Bitterli 2020 — direct lighting reservoir resampling |
| C.2 `svgf-denoiser` | NEW | XL | Schied 2017 + A-SVGF; denoiser abstraction layer for future NRD swap |
| C.3 `restir-gi` | NEW | XL | Ouyang 2021 — indirect bounce reservoirs |

**Phase C.5 — Path-traced reference mode**

| Effort | Issue | Size | Notes |
|---|---|---|---|
| C.5 `path-trace-reference` | NEW | M | Ground-truth PT mode reusing C.* infrastructure; validates RT GI convergence |

**Phase D — RT reflections + atmospheric polish**

| Effort | Issue | Size | Notes |
|---|---|---|---|
| D.1 `rt-reflections` | NEW | L | Stochastic ray reflections + denoise (supersedes planned SSR) |
| D.2 `volumetric-rt-shadows` | NEW | M | Shadow rays from voxel volume cells |
| D.3 `gpu-particles` | [#57](https://github.com/Hekbas/Luth/issues/57) | L | Compute sim; showcase-sized scope (fire / ember / smoke / motes in god ray) |

**Out of arc (deferred to follow-up series)**

- `character-shading` — skin (Jimenez15) + hair (Karis16 + Marschner03) + cloth (Estevez17). Triggered by adding a character to Bhaal Temple
- `gpu-driven` — mesh shaders + meshlet baker + HiZ occlusion (Framework 5 alignment)
- `virtual-geometry` — Nanite-class virtualized geometry; long-tail

### Gameplay enablement

| Pri | Epic | Issue | Target | Effort | Deps |
|---|---|---|---|---|---|
| 4 | `scripting` (C# or Lua) | NEW | v3.1.0 | XL | `rt-renderer` |
| 5 | `prefab-system` | NEW | v3.1.x | M | `scripting` |

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
