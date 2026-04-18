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

`IEditorHooks` exposes the editor-driven lifecycle, per-frame, project, input-capture, and viewport-snapshot calls the engine needs (Init / BeginFrame / Render / EndFrame / Shutdown / WantCaptureKeyboard / WantCaptureMouse / GetViewportState / OnProjectChanged / etc.).

`LuthienEditorHooks` in `luthien/source/luthien/EditorHooks.cpp` implements the interface by forwarding each call to the corresponding `Editor::` / `ProjectLauncher::` / `EditorSelection::` static API. Registration happens in `runtime/source/LuthienApp.cpp::CreateApp` via `InstallLuthienEditorHooks()` *before* `App::App()` runs — so the hook is live from the first engine call onward.

A runtime-only build that skips linking `Luthien.lib` leaves the registry empty and every engine-side `if (auto* h = EditorHooks::Get())` short-circuits cleanly.

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

Static singleton: `s_SelectedEntity`, `s_SelectedResource`, `s_Version`
- HierarchyPanel sets entity selection
- ProjectPanel sets resource selection
- InspectorPanel reads current selection
- ScenePanel uses selected entity for gizmo

## Command System (Command.h, CommandHistory.h)

**Undo/redo** via command pattern. `CommandHistory` is a static singleton with undo/redo stacks (max 100 entries).

**Execution flow:** `CommandHistory::Execute(cmd)` → `cmd->Execute()` → push to undo stack, clear redo stack. Supports merge (consecutive drags coalesce) and compound grouping (`BeginCompound`/`EndCompound`).

**Entity resolution:** All commands store `UUID`, never raw `entt::entity`. Entities are resolved via `Scene::FindEntityByUUID()` at execution time. This is critical because EnTT recycles entity handles — an entity destroyed and recreated (via undo of delete) gets a new handle but keeps its UUID.

**14 command types:** ComponentPropertyCommand (pointer-to-member), ComponentAddCommand, ComponentRemoveCommand, GizmoTransformCommand, EntityCreateCommand, EntityDestroyCommand (JSON subtree snapshot), EntityRenameCommand, EntityReparentCommand, EntityReorderCommand, EntityDuplicateCommand, ModelInstantiateCommand, MaterialSnapshotCommand, CompoundCommand.

**Shortcuts:** Ctrl+Z (undo), Ctrl+Y / Ctrl+Shift+Z (redo).

## Panels

### ScenePanel — 3D Viewport
- Displays `RenderingSystem::GetSceneColor()` texture
- **Editor Camera:** Orbit, pan, zoom, flythrough (WASD), entity tracking (F/Shift+F)
- **ImGuizmo gizmos:** Translate/Rotate/Scale with Ctrl-snap
- Fires `RenderResizeEvent` on viewport size change

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
