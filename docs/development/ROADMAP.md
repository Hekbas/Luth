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

---

## Planned Epics

Effort scale (scope/difficulty, not calendar time): **S** = small, contained · **M** = some design decisions · **L** = significant refactor or new system · **XL** = full new subsystem.

### Renderer pipeline (linear dependency chain)

| Pri | Epic | Issue | Target | Effort | Deps |
|---|---|---|---|---|---|
| 1 | `forward-plus` | [#54](https://github.com/Hekbas/Luth/issues/54) | v2.13.0 | L | `async-compute-queue` ✅ |
| 2 | `gpu-particles` | [#57](https://github.com/Hekbas/Luth/issues/57) | v2.14.0 | L | `forward-plus` |

### Gameplay enablement

| Pri | Epic | Issue | Target | Effort | Deps |
|---|---|---|---|---|---|
| 4 | `scripting` (C# or Lua) | NEW | v2.15.0 | XL | — |
| 5 | `prefab-system` | NEW | v2.15.x | M | `scripting` |

Scripting unblocks the `PlayerControllerSystem` stub deletion and is the prerequisite for most gameplay-side future ideas.

### Animation maturity

| Pri | Epic | Issue | Target | Effort | Deps |
|---|---|---|---|---|---|
| 6 | `animation-controller-v2` | [#94](https://github.com/Hekbas/Luth/issues/94) | v2.16.0 | XL | `animation-quick-pass` ✅ |

### Polish (no fixed slot — opportunistic)

| Epic | Issue | Effort | Notes |
|---|---|---|---|
| `procedural-sky` | NEW | M | Independent; drop into any quiet renderer slot |
| `jiggle-bones` | [#61](https://github.com/Hekbas/Luth/issues/61) | M | Benefits from `jolt-physics` ✅ colliders |
| `fxaa-taa` | [#72](https://github.com/Hekbas/Luth/issues/72) | M | TAA pairs with GTAO temporal accumulation |
| `rg-aliasing` (optional) | NEW | M | Defer unless `forward-plus` pressures transient VRAM |

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
