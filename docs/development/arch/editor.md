# Editor — Architecture Details

## Overview

ImGui-based editor with modular panel architecture, docking layout, asset drag-drop, and ImGuizmo gizmos. Renders as an ImGui pass at the end of the render graph.

Editor code lives in its own static library `Luthien.lib` at `luthien/source/luthien/` (since `arch-target-split` v2.0.0). The engine (`Luth.lib`) has no `luthien/...` includes — it reaches the editor only through the `IEditorHooks` interface.

## Editor Integration via `IEditorHooks`

The engine calls into the editor through a nullptr-safe hook registry declared in `luth/core/EditorHooks.h`:

```cpp
namespace Luth::EditorHooks {
    void Register(IEditorHooks* hooks);
    IEditorHooks* Get();  // nullptr in runtime-only builds
}
```

### Hook surface

| Method | Called from | Purpose |
|---|---|---|
| `Init(Window*)` / `Shutdown()` | `App::Init` / `App::~App` | Editor lifecycle |
| `BeginFrame()` / `Render()` / `EndFrame()` | `App::Run` | Per-frame ImGui pump |
| `WantCaptureKeyboard()` / `WantCaptureMouse()` | `Input::IsKeyPressed` / mouse polling | Gates input when ImGui has focus |
| `GetPlayState() → PlayState` | `PhysicsSystem::Update`, `App::Run` | Read editor's transport state |
| `ConsumeStepRequest() → bool` | `App::Run` | Frame-step in Paused mode (one-shot) |
| `GetViewportState(EditorViewportState&)` | `App::Run` (game-stage prep) | Snapshot debug toggles + selected entities |
| `OnProjectChanged()` | `App::LoadProject` | Reload panels, refresh project-aware state |
| `OnFrameDebuggerNotice(string)` | RenderPipeline | Surface frame-debugger banner to ConsolePanel |

`LuthienEditorHooks` in `luthien/source/luthien/EditorHooks.cpp` implements the interface by forwarding each call to the corresponding `Editor::` / `ProjectLauncher::` / `EditorSelection::` static API. Registration happens in `runtime/source/LuthienApp.cpp::CreateApp` via `InstallLuthienEditorHooks()` *before* `App::App()` runs — so the hook is live from the first engine call onward.

A runtime-only build that skips linking `Luthien.lib` leaves the registry empty and every engine-side `if (auto* h = EditorHooks::Get())` short-circuits cleanly.

### `PlayState` (since v2.8.0)

```cpp
enum class PlayState { Editing, Playing, Paused };
```

The engine reads this per-frame and computes `m_RunGameSystems` — gameplay systems (`AnimationSystem`, `PlayerControllerSystem`) only tick when the result is true:

```
m_RunGameSystems = !haveEditor
                || (playState == Playing)
                || (playState == Paused && stepThisFrame)
                || (playState == Editing && viewState.previewAnimationInEditor);
```

`PhysicsSystem` runs unconditionally but early-returns from its substep loop in Editing mode — bodies still exist, debug-draw still renders, lifecycle queues still drain (so the user can author colliders without entering Play), but no `JPH::PhysicsSystem::Update` call.

### `EditorViewportState`

Per-frame snapshot of editor toggles the engine needs to read. Populated by `GetViewportState`. Fields cover: selected entities (for outline + gizmo), preview-animation-in-editor flag, physics debug toggles (shapes/AABBs/CoM per Selected/All scope + color mode + alpha + segment count), frame-debugger capture state.

## Core (Editor.h/.cpp)

**Lifecycle:** `Init(Window*)` → `BeginFrame()` → `Render()` → `EndFrame()` → `Shutdown()`

- Creates ImGui context with GLFW + Vulkan backends
- Enables docking and viewports
- Loads fonts (main + FontAwesome icons)
- Creates and registers all panels

**Two-phase startup:**
- Phase 1: Editor initializes with ImGui + panels. No project needed. ProjectPanel shows "No project loaded".
- Phase 2: When a project is loaded via `Editor::OnProjectChanged()`, panels refresh, settings reload, and the editor enters normal operation.

**Scene management:** `NewScene()`, `OpenScene(path)`, `SaveScene()`, `SaveSceneAs()`
- Dirty detection via hierarchy version tracking
- Keyboard shortcuts: Ctrl+N/O/S, Ctrl+Shift+S

**Project management:** `ShowProjectLauncher()`, `OnProjectChanged()`
- File menu: Open Project, Project Launcher
- `OnProjectChanged()` — reloads editor settings, clears scene, refreshes ProjectPanel and HierarchyPanel

**Frame integration:** `App::Run()` calls `EditorHooks::Get()->BeginFrame()`, then `OnUpdate()` (app override), then `EditorHooks::Get()->Render()` and `EndFrame()`. The hook impl forwards each to the corresponding `Editor::*` static. ImGui draw data is consumed by `RenderPipeline::AddImGuiPass()` in the render graph (moved from `RenderingSystem` in `arch-renderer-split` v1.7.0). `ImGui::UpdatePlatformWindows` / `RenderPlatformWindowsDefault` run inside `Editor::EndFrame` when viewports are enabled.

## UI Utilities (UI.h/.cpp)

**Property widgets** for the inspector (2-column layout):
- `Property(label, T&)` — text, bool, int, float, Vec2/Vec3/Vec4 with colored axis buttons
- `PropertyColor()` — color picker
- `PropertyAsset(label, UUID&, AssetType)` — asset slot with drag-drop acceptance + type validation

**Texture rendering:** `GetTextureID(texture)` creates and caches `VkDescriptorSet` via `ImGui_ImplVulkan_AddTexture`. Uses weak_ptr tracking for auto-cleanup.

## Selection System (EditorSelection.h)

Static singleton holding a multi-selection set: `s_SelectedEntities` (vector; primary = last added), `s_SelectedResource`, `s_Version`. Mutators (`SelectEntity` replace / `AddEntity` / `ToggleEntity` / `RemoveEntity` / `ClearSelection`) bump the version and publish `SelectionChangedSignal`.
- HierarchyPanel: Ctrl-click toggles, Shift-click selects the span to the anchor in visible-row order
- ProjectPanel sets resource selection
- InspectorPanel edits the primary; with 2+ selected, edits fan across the set via the `MultiEdit` broadcast context (per-member edits + Reset/Remove/Add/Active; combos primary-only)
- ScenePanel: a plain viewport click selects a model's root first, then the exact clicked part once that model is active (Ctrl/Shift extend/toggle the exact entity); gizmo transforms the whole selection about the active (primary) pivot as one compound undo (root-filtered); LMB-drag marquee select picks entities by screen-projected origin (Shift adds / Ctrl toggles); batch delete (`EntityDestroyMultipleCommand`) removes the whole selection as one undo

## Command System (Command.h, CommandHistory.h)

**Undo/redo** via command pattern. `CommandHistory` is a static singleton with undo/redo stacks (max 100 entries).

**Execution flow:** `CommandHistory::Execute(cmd)` → `cmd->Execute()` → push to undo stack, clear redo stack. Supports merge (consecutive drags coalesce) and compound grouping (`BeginCompound`/`EndCompound`).

**Entity resolution:** All commands store `UUID`, never raw `entt::entity`. Entities are resolved via `Scene::FindEntityByUUID()` at execution time. This is critical because EnTT recycles entity handles — an entity destroyed and recreated (via undo of delete) gets a new handle but keeps its UUID.

**14 command types:** ComponentPropertyCommand (pointer-to-member), ComponentAddCommand, ComponentRemoveCommand, GizmoTransformCommand, EntityCreateCommand, EntityDestroyCommand (JSON subtree snapshot), EntityRenameCommand, EntityReparentCommand, EntityReorderCommand, EntityDuplicateCommand, ModelInstantiateCommand, MaterialSnapshotCommand, CompoundCommand.

**Shortcuts:** Ctrl+Z (undo), Ctrl+Y / Ctrl+Shift+Z (redo).

## Workspaces (since v2.9.7)

Named editor layouts. Each workspace pairs an ImGui dock-state file (`<name>.ini`) with a per-panel visibility set (sidecar `<name>.workspace.json`). User-created workspaces live at `<project>/.luth/layouts/`; the built-in `Default` ships at `luth/assets/workspaces/Default.{ini,workspace.json}` (read-only — built-in shadows user copy of same name).

**API (static on `Editor`):** `LoadWorkspace(name)` · `SaveWorkspaceAs(name)` · `RenameWorkspace(old, new)` · `DeleteWorkspace(name)` · `ResetWorkspaceToBuiltin(name)` · `GetWorkspaces() → vector<string>` · `SaveActiveWorkspaceSidecar()`.

**Persistence flow:** `Editor::Shutdown` calls `SaveActiveWorkspaceSidecar` after `SaveSettings` so live `panel->m_Open` flips persist into the sidecar (built-in active = no-op; built-ins are read-only). Active workspace name persists in `EditorSettings`; `s_NeedActiveWorkspaceLoad` defers the first apply to end of Render so panels + ImGui dock state exist before reload.

**UI surfaces:** `Window > Workspaces` lists workspaces with Switch / Save As / Rename / Delete / Reset to Builtin (Rename + Delete disabled when active is built-in). `Window > <panel name>` toggles `m_Open` per panel (separate from per-frame `m_Visible`). `Window > Reset Layout` loads `layouts/Default.ini`.

**Signals:** `WorkspaceChangedSignal` on `BusType::MainThread` (no in-tree subscribers yet — future-proof hook).

## Panels

### ScenePanel — 3D Viewport
- Displays `RenderingSystem::GetSceneColor()` texture
- **Editor Camera:** Orbit, pan, zoom, flythrough (WASD), entity tracking (F/Shift+F)
- **ImGuizmo gizmos:** Translate/Rotate/Scale with Ctrl-snap
- Per-instance resize callback (`ViewportRenderer::SetOnResize`) — resizes `RenderingSystem` + `EditorCamera`

### HierarchyPanel — Entity Tree
- Hierarchical tree view with expand/collapse
- Search filter (substring match)
- Drag-drop: reparent entities, drop models from ProjectPanel
- Context menu: create entity, delete, rename (F2)
- Deferred action queue to safely modify scene during iteration

### InspectorPanel — Properties
- **Entity mode:** Transform, Camera, MeshRenderer, Animation, Lights, Add Component button
- **Resource mode:** Material editor, Model viewer, Texture editor (wrap/filter/mipmap settings)
- Template `DrawComponent<T>()` pattern for each component type

### ProjectPanel — Asset Browser
- **Left:** Directory tree (recursive folders)
- **Right:** File grid with type-based icons and thumbnails
- Drag-drop source: sends `ASSET_UUID` payload
- Context menu: create folder/material, delete, rename
- Search: real-time recursive case-insensitive
- Guards against no-project state: shows "No project loaded" placeholder when `FileSystem::HasProject()` is false

### ProjectLauncher — Startup Project Selector
- ImGui overlay, shown when no project is auto-discovered on startup
- Also accessible via `File > Project Launcher...`
- **Header:** Luth logo + "Projects" title + "Add" (open existing) + "+ New" (create) buttons
- **Recent projects:** Stored in `%APPDATA%/Luth/recent_projects.json`, max 10 entries
- **Project rows:** Name, relative time ("7 hours ago"), version, path
- **New Project dialog:** Name + location, creates directory structure + `.luthproj`
- **Interaction:** Double-click a project to open it. Right-click to remove from list.
- **Pending mechanism:** Sets a pending path consumed by `App::LoadProject()` in the main loop
- **Drag-drop:** `.luthproj` files dropped on the editor window trigger a project switch

### RenderPanel — Post-Processing Controls
- Bloom: threshold, strength
- Tonemapping: operator (Linear/Reinhard/ACES/Uncharted2), exposure, contrast, saturation
- Vignette: amount, hardness
- Effects: grain, chromatic aberration
- Reads/writes `RenderingSystem::GetPostProcessSettings()`

### ProfilerPanel — Performance
- FPS/frame time graph (100 frames)
- Job system stats: worker count, fiber usage, queue load
- Memory: per-category current/peak, GPU memory (VMA), leak detection
- Updates at 10 Hz

### FrameDebuggerPanel — Render Graph
- Pass tree with per-pass GPU timing
- Event slider for pass selection
- Pipeline state table, resource list, texture preview
- Reads `RenderGraphSnapshot` captured each frame

### ResourcePanel — Asset Database Inspector
- Table: Name, Type, UUID, Reference Count
- Type filters (checkboxes) + search

### HistoryPanel — Undo/Redo Debug
- Timeline-style visualization of undo/redo stacks
- Per-command type icons, expandable compound commands
- Undo/Redo/Clear buttons with stack size counters

### ConsolePanel — Log Viewer (since v2.9.2)
- Implements `ILogSink`: sink callback (any thread) enqueues `LogEntrySignal` on the main `EventBus`; handler appends to a capped deque (1024)
- Level filter (trace/info/warn/error) + case-insensitive search + auto-scroll
- `ImGuiListClipper` row loop for log-explosion resilience
- Per-panel error boundary on `OnDraw` (3-strike crash gate, then placeholder window with manual Reset)

### GamePanel — Runtime View (since v2.8.1)
- Dedicated camera-driven viewport rendering the scene's first `Component::Camera` entity
- Letterbox + no overlays (no gizmo, no outline) — matches what a built runtime would show
- Uses `RenderView` + `ViewResources` cache; per-instance resize callback replaces the old global `RenderResizeEvent`

### EditorSettingsWindow — Preferences (since v2.9.7, NOT a `Panel`)
- Standalone window opened from `Edit > Preferences`, centered on engine window
- Unity-style two-pane: left section list with resizable splitter, right scrollable body
- Top-right search filters rows across all sections
- Commits trigger `SaveSettings` + `ApplyPersistence` so camera/skybox propagate live

### TextureRemapDialog — Asset Import Helper (since `smart-import-hot-reload` v1.0.0)
- Modal triggered by `ImportReport` when texture discovery finds ambiguous candidates
- Per-texture combo of detected candidates + browse fallback
- Commits write to the model's `.meta` and trigger a reimport
- Undo/Redo/Clear buttons with stack size counters
