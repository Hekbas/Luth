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
| v2.8.8 | `animation-quick-pass` | `AnimationClip` becomes a first-class UUID-addressable asset. `ModelImporter` writes one `.anim` per clip into `<stem>_Animations/` (mirrors materials/textures); `AnimationClipImporter` cooks them. `Animation::ClipUUID` and `BlendLayer::ClipUUID` replace integer indices; scene loader migrates legacy `animationIndex` via blocking model load. Drawers swap `ImGui::Combo` for `UI::PropertyAsset` drag-drop. AssetDatabase Modified now evicts the in-memory copy so `.anim` hot-reload takes effect live. Foundation for `animation-controller-v2` — bone-name retargeting still v2.11 scope | 2026-04-28 |
| v2.8.9 | `persistent-buffer-ring` | Triple-buffered the three persistent CPU-mapped SSBOs (ObjectSSBO Set 5, IndirectBuffer, Material SSBO Set 2) so frame N writes never overlap frame N-1's GPU reads. Single VkBuffer per resource sized 3×, slice base baked into `firstInstance` / cull `destOffset` / `obj.materialIndex` — zero shader changes, descriptors stay `VK_WHOLE_SIZE`. Cull compute gains a `srcOffset` push-constant for the active object slice (push range 104B→108B). Material dirty-frame countdown propagates a single mutation to all slices over MAX_FRAMES_IN_FLIGHT iterations. VMA modernized off deprecated `CPU_TO_GPU` to `AUTO + HOST_ACCESS_SEQUENTIAL_WRITE_BIT + MAPPED_BIT` with `vmaFlushAllocation` per slice. Tag-only | 2026-04-28 |
| v2.8.10 | `gpu-tagged-heap` | GPU half of the Naughty Dog Onion/Garlic split: `Memory::GPUTaggedPageAllocator` (sibling to CPU `TaggedPageAllocator`) vends 2 MB pages from 64 MB host-mapped backings, tag-based bulk-free wired to GPU-N-2 timeline completion in `AcquireImage`. Material / Object / Indirect / `BoneMatrixBuffer` all flow through it; v2.8.9's slot-encoded ring buffers and the `gpu_cull.comp` `srcOffset` push-constant dissolve (push range 108B→104B). Sets 2/4/5 + cull descriptor rebind per-frame to allocator-returned regions (UPDATE_AFTER_BIND). CPU `TaggedPageAllocator` V6 wiring also completed — `FreeTag` had zero callsites and `JobContext::Allocator` was unassigned. ProfilerPanel surfaces tagged-heap stats. Tag-only | 2026-04-29 |
| v2.8.11 | `slot-alloc-spinlock` | Closed the v2.8.4 D6 carry-over: `MaterialSystem::m_Lock` and `BoneMatrixBuffer::m_Lock` `std::mutex` → `Luth::SpinLock`. With per-frame upload moved off the lock in `gpu-tagged-heap`, critical sections shrink to slot-alloc paths only. Doc sweep folded in (`arch/memory.md` adds GPU heap; `arch/rendering-pipeline.md` updates Set 2/4/5 rebind cadence; `arch/fiber-system.md` notes both halves of Onion/Garlic operational). Tag-only | 2026-04-29 |
| v2.8.12 | `shader-reload-async` | Drops the per-save `vkDeviceWaitIdle` from the shader hot-reload path. Reload callback now builds new pipelines first, pushes old `VKPipeline`/`VKComputePipeline` to `VulkanContext::PushDeletion` (V1 SpinLock-safe per v2.8.7) — drained MAX_FRAMES_IN_FLIGHT frames later in `AcquireImage`. `VulkanShader::Reload` drops its own redundant `vkDeviceWaitIdle` (VkShaderModule is consumed at pipeline-create per spec; old pipelines hold no module reference). `PipelineManager` gains `DeferredClear()` / `DeferredInvalidateShader()` for the cached PBR variants. `m_ShaderWatcher.Poll()` moved from per-`Execute` to once-per-frame in `RenderingSystem::Update`. Net: shader save no longer drops a frame. Tag-only | 2026-04-29 |
| v2.8.13 | `vulkan-polish` | Tier-2/3 cleanup before `jolt-physics`. Validation messenger pNext-chained for instance create/destroy coverage; `BindlessDescriptorSet` free-list switched to `vector<u32>` LIFO with `INVALID_BINDLESS_SLOT` sentinel disambiguating "not registered" from the reserved null-texture slot 0; `RenderResourceCache` keyed on `unordered_multimap<u64,…>` with `(w, h, format, usage)` and stale threshold 10000→30; runtime buffer uploads routed through `UploadContext::UploadBuffer` (texture half deferred to v2.8.14); outline + grid push-constant literals plumbed through `EditorSettings`/`EditorViewportState`/`CameraParams`; vestigial `DescriptorAllocator` removed (IBLPrecompute owns a local pool now). Tag-only | 2026-04-29 |
| v2.8.14 | `texture-async-uploads` | Finishes the texture half of `vulkan-polish` S4. New `UploadContext::UploadImageMipped` records pre-barrier all mips → mip-0 staging copy → `vkCmdBlitImage` chain → final SHADER_READ_ONLY in one cmd-buffer; 4-slot cmd-buffer ring inside `UploadContext` removes the F3 pre-reset fence wait so submits overlap on the GPU. Deferred-bindless-registration pump composes with `AssetManager::s_UploadQueue` main-thread tick — `VKTexture` ctor pushes `{outIndex, view, sampler, fence}`, pump checks `IsComplete` per frame and calls `BindTexture` once ready; until then `INVALID_BINDLESS_SLOT` + `Material::BindlessOrNull` keeps materials sampling reserved slot 0 (white fallback). `~VKTexture` cancels pending entries by view-handle match. Inline `ImmediateSubmit` lambda (~100 LOC) in `VKTexture::CreateImage` data path retired; the 5 sync init/control-flow sites untouched. Tag-only | 2026-04-30 |
| v2.9.0 | `editor-foundation` | First effort of the AAA editor rework. Replaces the bare `OnInit`/`OnRender` Panel contract with a Gather→Draw lifecycle: panel data collection runs in parallel on worker fibers via `JobSystem::Execute`; ImGui submission stays on main reading frozen, immutable per-panel snapshots. Per-panel `LinearAllocator(64*1024)` gather scratch; `TaggedPageAllocator` rejected during Phase 3 review for pages-held-in-fiber-cache leaks. 9 panels migrated through a `UsesNewLifecycle` bridge sentinel (sentinel + legacy `OnRender` stripped in K). Panel introspection (`m_Visible`/`m_Focused`/`m_Docked`) populated by new `Panel::BeginWindow` helper. Editor functionally identical to v2.8.14. Milestone Release | 2026-05-01 |
| v2.9.1 | `editor-signal-bus` | Layers typed `EditorSignal` events on the existing `EventBus::BusType::MainThread` so panels react to selection / hierarchy / asset / project / play-state via subscriptions instead of polling. Replaces `Scene::GetHierarchyVersion` polling block in `Editor::Render` with `HierarchyChangedSignal` subscription. Bundles `EventBus` hardening (pre-effort audit verdict YELLOW): exception-safe dispatch; `SubscriptionHandle` + `Unsubscribe` for panel-lifetime safety; tracked allocations via `EventDeleter` + `MemoryTracker`; thread assertion on `ProcessEvents`. Five signals (Selection/Hierarchy/Asset/Project/PlayState), all UUID-based per the v2.7.0 command precedent. `EditorSelection` split header/cpp so signal includes don't leak through wide accessors. Tag-only | 2026-05-02 |
| v2.9.2 | `editor-console-errors` | Bundles two pillars from the editor-aaa plan. New `Log::AddSink` / `ILogSink` interface with internal `ForwardingSink` (spdlog `base_sink`) fanning to registered sinks under `Luth::SpinLock`. New `ConsolePanel` implements `Panel` + `ILogSink`: sink callback (any thread) enqueues `LogEntrySignal` on main bus, handler appends to capped deque (1024); level filter + case-insensitive search + auto-scroll + `ImGuiListClipper`. Per-panel error boundary on `OnDraw` mirrors gather thunk's catch contract: `m_CrashStreak >= 3` flips `m_Crashed`, panel goes dark behind a placeholder window with manual `Reset`. Stack-trace dump (Win32 DbgHelp, `dbghelp.lib` already linked) on every catch. Tag-only | 2026-05-02 |
| v2.9.3 | `editor-job-pump` | New `Luth::MainThreadPump` static facade — `Post(Callback)` from any thread, `Drain()` on main, `PendingCount()` diagnostic. Storage `std::queue<std::function<void()>>` under `std::mutex`, mirroring v2.9.1-hardened EventBus shape: swap-and-drain, debug thread-assert latched on first Drain, per-callback `try/catch`, `Memory::Category::Editor` accounting outside the lock. Drain wired in `App::Run` at L178 after `EventBus::ProcessEvents` and before `EditorHooks::BeginFrame` so callbacks mutate state, not ImGui mid-frame. `AssetManager::s_UploadQueue` deliberately not migrated — typed pipeline stage with mid-iteration `LoadAsync` recursion + GPU-fence ordering, not opaque callback erasure. Foundation for `editor-autosave` (v2.9.4) and `editor-thumbnails` (v2.9.5). Tag-only | 2026-05-02 |
| v2.9.4 | `editor-autosave` | First real consumer of `MainThreadPump`. Periodic side-channel autosave to `<project>/.luth/autosaves/<stem>-<TS>.luth` — never the canonical scene; dirty `*` persists until manual Save. New `Luth::EditorAutoSave` static module (`Init/Shutdown/Tick/ForceNow/ScanForRecovery/DrawRecoveryModal`); JSON snapshot via `SceneSerializer::SaveToString` on main (V3-anchored), file write via `IOThread::WriteFile`, completion + prune via `MainThreadPump::Post`. Play-mode gate via `PlayStateChangedSignal`. Lazy timer init (Time::Update lags Editor::Init). Crash-recovery prompt: scan in `OpenScene` covers auto-load + manual paths; modal Recover/Discard/Cancel. `EditorSettings` extension (`autoSaveEnabled`, `autoSaveIntervalSec`, `autoSaveKeepN`) + `File > Autosave Now` + fading title-bar `Autosaved HH:MM` suffix. Drive-by: scene auto-load relocated from `SetActiveScene` (ran before `LoadProject`) to `OnProjectChanged` — long-broken auto-load works again as a side effect. Tag-only | 2026-05-03 |
| v2.9.5 | `editor-thumbnails` | ProjectPanel grid switches from FA-icon-only to rendered previews for textures, meshes, materials. New `widgets/ThumbnailCache` (UUID-keyed, SpinLock-guarded map + completion queue + dispatch queue, `AssetChangedSignal` cascade invalidation for material→texture deps), `widgets/ThumbnailGenerator` (worker-fiber CPU bake for textures via stbi_load → stbir_resize → stbi_write_png; main-thread synchronous GPU bake for mesh + material via `VulkanContext::ImmediateSubmit`), `widgets/ThumbnailPreviewScene` (custom Lambert + ambient × albedo shader, two pipeline variants for static/skinned vertex strides, persistent 128² color RT + D32 depth + host-mapped staging, lazy-loaded Sphere primitive for material bakes, direct sampler binding bypasses bindless registration race). Disk-persisted at `<project>/.luth/thumbnails/<uuid>.png`; per-frame budgets (5 texture / 1 GPU bake) keep cold-start smooth. Drive-bys: new `Luth::Image` module centralises every stb_image touch (one site sets the global flip flag, no race); deleted dead `VKTexture(const fs::path&)`; engine-wide fix — `VulkanContext::FlushAllDeletionQueues` before `ImGui_ImplVulkan_Shutdown` so PushDeletion lambdas don't fire against a destroyed pool at close. ProjectPanel layout polish: full-cell `Selectable` (selection visual + click + drag-drop), 3-line truncated names, list-view actual thumbnails. Tag-only | 2026-05-03 |
| v2.9.6 | `editor-undo-fix` | Slider-driven inspector edits stop over-coalescing across release boundaries. New `EditState { changed, committed, itemId }` returned from every `UI::Property*` + `PropertyAsset` (`operator bool` keeps existing `if (UI::Property(...))` call sites compiling); per-T `unordered_map<ImGuiID, T>` Meyers-singleton stash holds the pre-edit value between `IsItemActivated` and `IsItemDeactivatedAfterEdit`. `ComponentPropertyCommand::CanMerge` and `VectorElementPropertyCommand::CanMerge` deleted — every release-after-edit boundary = one undo entry. 8 component drawers migrated to push on `state.committed`; direct `ImGui::SliderFloat` sites in `AnimationControllerDrawer` use the inline activate/deactivate pattern. Discrete widgets (Checkbox / Combo / PropertyAsset) commit synchronously on `changed` because ImGui's `IsItemDeactivatedAfterEdit` is unreliable when activation + edit + deactivation collapse into one frame. `MaterialEditor` debounced `MaterialSnapshotCommand` deliberately untouched — different mechanism, already correct. Tag-only | 2026-05-04 |
| v2.9.7 | `editor-panels-polish` | New `widgets/ButtonGroup` (`SegmentedButton` + `IconToggleGroup` + `IconToggleButton` + Unity-style `SplitToggleButton` with caret-anchored popup). ScenePanel toolbar fully re-laid: gizmo `IconToggleGroup` + Grid split (left), centered transport, 4 sphere-icon render modes (GLOBE/EARTH_AMERICAS/CIRCLE/CIRCLE_HALF_STROKE — ShadedWireframe stubbed disabled) + Debug/Camera/Gizmos splits + controls overlay (right); Debug-split icon toggles current<->`lastDebugMode`, chevron radios Normals/EntityID. Tri count moved to top-right viewport overlay. Gizmos split icon is a master toggle (save/restore per-flag state). HierarchyPanel per-row visibility eye via `SmallButton` overlapping the TreeNode (`AllowItemOverlap`); hover-based selection gating because TreeNode `IsItemClicked` fires on press while `SmallButton` fires on release; inactive rows dim text+icon via `ImGuiCol_TextDisabled`. ProjectPanel grid migrates from `ImGui::Columns` to `BeginTable` + `ImGuiListClipper` over a per-frame flat entries vec. New `panels/EditorSettingsWindow` (standalone, NOT a `Panel`) opened from `Edit > Preferences` — Unity-style two-pane (left section list with resizable splitter, right scrollable body), centered on engine window, top-right search filters rows across all sections; commits trigger `SaveSettings` + `ApplyPersistence` so camera/skybox propagate live (`ApplyPersistence` promoted to public). New `Window` menu lists every panel and toggles `&Panel::m_Open` (separate from per-frame `m_Visible`); `Reset Layout` loads `layouts/Default.ini` (auto-saved on first run when missing). `EditorSettings::panelOpen` (string→bool, keyed by `Panel::GetWindowID()`) hydrates `m_Open` in `ApplyPersistence` and is mirrored back inside `SaveSettings`. Each panel ctor now sets a unique `m_WindowID` so the Window menu doesn't trip ImGui's "10 conflicting ID" warning. Edit menu adds `Duplicate` / `Delete` items wrapping existing entity commands; `Ctrl+D` shortcut added to `ProcessShortcuts`, `Del` continues to live in HierarchyPanel-focused path. ResourcePanel gains dirty-tracked rebuild cache (`m_NeedsRebuild` flips on AssetDatabase change / search / filter / sort spec dirty), `ImGuiListClipper` row loop, and `RefreshDynamicData` per-frame RefCount refresh with conditional re-sort when sorting by Refs. Engine drive-by: `Scene::DuplicateEntity` no longer double-pushes root duplicates into `m_RootEntities` (already added by `CreateEntity`) and no longer leaves parented duplicates orphaned in roots — pre-existing bug surfaced by the new `Ctrl+D` path. Cut/Copy/Paste deferred — no `EditorClipboard` exists. Tag-only | 2026-05-04 |

---

## Planned Epics

Effort scale (scope/difficulty, not calendar time): **S** = small, contained · **M** = some design decisions · **L** = significant refactor or new system · **XL** = full new subsystem.

| Priority | Epic | Issue | Target | Effort | Deps |
|----------|------|-------|--------|--------|------|
| 1 | `editor-inspector-polish` | NEW | v2.9.8 | M | `editor-undo-fix`, `editor-thumbnails` |
| 2 | `editor-workspaces` | NEW | v2.9.9 | M | `editor-foundation` |
| 3 | `frame-debugger-replay-extend` | [#100](https://github.com/Hekbas/Luth/issues/100) | v2.9.x | S–M | `frame-debugger-polish` |
| 4 | `jolt-physics` | [#56](https://github.com/Hekbas/Luth/issues/56) | v2.10.0 | XL | `play-mode`, `editor-workspaces` |
| 5 | `jiggle-bones` | [#61](https://github.com/Hekbas/Luth/issues/61) | v2.10.1 | M | — |
| 6 | `async-compute-queue` | NEW | v2.10.2 | L | `vulkan-correctness` |
| 7 | `rg-aliasing` (optional) | NEW | v2.10.3 | M | — |
| 8 | `procedural-sky` | NEW | v2.10.4 | M | `jolt-physics` |
| 9 | `forward-plus` | [#54](https://github.com/Hekbas/Luth/issues/54) | v2.11.0 | L | `compute-gpu-culling`, `async-compute-queue` |
| 10 | `fxaa-taa` | [#72](https://github.com/Hekbas/Luth/issues/72) | v2.11.1 | M | — |
| 11 | `animation-controller-v2` | [#94](https://github.com/Hekbas/Luth/issues/94) | v2.12.0 | XL | `animation-quick-pass` |
| 12 | `gpu-particles` | [#57](https://github.com/Hekbas/Luth/issues/57) | v2.13.0 | L | `compute-gpu-culling`, `forward-plus` |

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
