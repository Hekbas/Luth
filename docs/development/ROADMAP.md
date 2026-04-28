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
| v2.8.0 | `play-mode` | Editor state machine (Editing/Playing/Paused) + JSON scene snapshot. AnimationSystem gated; CommandHistory blocks during play; transport bar + viewport tint | 2026-04-23 |
| v2.8.1 | `game-panel` | Dedicated Game panel rendering first Camera entity with letterbox + no overlays. New `RenderView` + `ViewResources` cache; per-instance resize callback replaces `RenderResizeEvent` | 2026-04-24 |
| v2.8.2 | `engine-consolidation` | Audit-driven housekeeping: roadmap restructure + 4 new arch docs (memory/profiling/validation-layers/version-glossary) + comment-banner sanitization (14 files, –104 LOC) + Tracy global memory hooks for STL/heap + Tracy CPU coverage gaps filled | 2026-04-25 |
| v2.8.3 | `tracy-on-demand` | Hotfix: define `TRACY_ON_DEMAND` so Tracy macros no-op when no profiler client is connected. Fixes 10 MB/s launcher leak + 0.2 MB/s in-game leak (#30); uncovered after v2.8.2 wired global `new`/`delete` to Tracy | 2026-04-25 |
| v2.8.4 | `pipeline-phase-3` | Game(N) and Render(N-1) actually run concurrently on worker fibers. `RenderSnapshot` POD frozen at end of game stage; render reads it, never the registry. Two-phase RG dispatch (1 yield/frame). Stage-isolation asserts on retained-mutex subsystems. Four latent JobSystem bugs fixed along the way (deque race, FP fiber state, registry RTTI scan, fiber pinning) | 2026-04-26 |
| v2.8.5 | `build-config-foundation` | `luth/core/BuildConfig.h` centralizes build-config detection: `LUTH_BUILD_DEBUG/RELEASE/DIST` (premake-set) + derived `LUTH_ENABLE_VALIDATION` / `LUTH_SPIRV_CROSS_ENABLED`. Engine no longer tests `_DEBUG`/`NDEBUG`/bare `DEBUG`. Fixes Vulkan validation leak in Release. First effort under tag-only release policy | 2026-04-27 |
| v2.8.6 | `frame-debugger-polish` | Unity-style draw-scrub slider + tree-click snap; viewport pass overlay (Scene/Game-coupled to capture source); depth archives in viewport via tonemapped preview; archive image reuse + 10 Hz throttle eliminating editor freezes under continuous camera movement; per-draw replay dispatch refactor (Shadow/Depth/Selection bodies deferred to #100); pass-archive index keying fix + graph-order pass sort + stable EventTree IDs along the way | 2026-04-28 |
| v2.8.7 | `vulkan-correctness` | Tier-1 Vulkan hardening: deletion-queue SpinLock (V1), RG WAW barrier emission, transitive producer revival fix, swapchain OUT_OF_DATE handling + deferred Present rebuild (V2), device Features2 capability validation, sync2 frame submit + RG-driven present transition (postBarriers). 11 commits; smoke test surfaced a 240 ms SUBOPTIMAL stall + std::mutex-vs-SpinLock; arch audit fixed timeline monotonicity + render-fiber `vkDeviceWaitIdle` | 2026-04-28 |

---

## Planned Epics

Effort scale (scope/difficulty, not calendar time): **S** = small, contained · **M** = some design decisions · **L** = significant refactor or new system · **XL** = full new subsystem.

| Priority | Epic | Issue | Target | Effort | Deps |
|----------|------|-------|--------|--------|------|
| 1 | `animation-quick-pass` | [#93](https://github.com/Hekbas/Luth/issues/93) | v2.8.8 | S | — |
| 2 | `persistent-buffer-ring` | NEW | v2.8.9 | S–M | `vulkan-correctness` |
| 3 | `shader-reload-async` | NEW | v2.8.10 | S | `vulkan-correctness` |
| 4 | `vulkan-polish` | NEW | v2.8.11 | M | `vulkan-correctness` |
| 5 | `frame-debugger-replay-extend` | [#100](https://github.com/Hekbas/Luth/issues/100) | v2.8.x | S–M | `frame-debugger-polish` |
| 6 | `jolt-physics` | [#56](https://github.com/Hekbas/Luth/issues/56) | v2.9.0 | XL | `play-mode` |
| 7 | `jiggle-bones` | [#61](https://github.com/Hekbas/Luth/issues/61) | v2.9.1 | M | — |
| 8 | `async-compute-queue` | NEW | v2.9.2 | L | `vulkan-correctness` |
| 9 | `rg-aliasing` (optional) | NEW | v2.9.3 | M | — |
| 10 | `forward-plus` | [#54](https://github.com/Hekbas/Luth/issues/54) | v2.10.0 | L | `compute-gpu-culling`, `async-compute-queue` |
| 11 | `fxaa-taa` | [#72](https://github.com/Hekbas/Luth/issues/72) | v2.10.1 | M | — |
| 12 | `animation-controller-v2` | [#94](https://github.com/Hekbas/Luth/issues/94) | v2.11.0 | XL | `animation-quick-pass` |
| 13 | `gpu-particles` | [#57](https://github.com/Hekbas/Luth/issues/57) | v2.12.0 | L | `compute-gpu-culling`, `forward-plus` |

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
- Animation v3: DQS, morph targets, IK, animation LODs (post-`animation-controller-v2`)
- Prefab system (reusable entity templates)
- Scripting (C# via Mono, or Lua)

### Audio
- 3D spatial audio
- Audio asset pipeline

### Editor & Tools
- Asset streaming (async GPU upload pipeline)
- Visual shader editor
