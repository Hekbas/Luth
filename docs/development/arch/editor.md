# Editor — Architecture Details

## Overview

ImGui-based editor with modular panel architecture, docking layout, asset drag-drop, and ImGuizmo gizmos. Renders as an ImGui pass at the end of the render graph.

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

**Frame integration:** Editor::Render() is called during App::OnUpdate(). ImGui draw data is consumed by `RenderingSystem::AddImGuiPass()` in the render graph.

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
