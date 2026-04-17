# Phase 5-J: Scene Serialization ✅ (2026-03-19)

**Goal:** Save/load scenes to/from JSON `.luth` files with full component serialization, native file dialogs, and editor integration.

### Implementation

`SceneSerializer::Save/Load` uses depth-first entity traversal with hierarchy reconstruction via parent UUIDs. All components serialized: Transform, Camera, MeshRenderer, Animation, DirectionalLight, PointLight.

Editor integration: File menu (New/Open/Save/Save As), Ctrl+N/O/S/Shift+S shortcuts, dirty tracking via hierarchy version, window title asterisk indicator. ProjectPanel supports double-click `.luth` to load.

### Bug Fixes

1. **Scene::Clear() sparse-set crash** — destroying all entities at once violated EnTT sparse-set bucket invariants. Fixed by entity-by-entity destruction.
2. **Dangling Systems::s_Scene** — `HierarchyPanel` created a temporary Scene that was destroyed when `SetActiveScene` replaced context. `SetActiveScene` now syncs `Systems::SetScene`.
3. **DestroyEntity UB** — entity name was accessed after `registry.destroy()`. Fixed by saving name before destruction.

### Files

| File | Changes |
|---|---|
| `luth/scene/SceneSerializer.h/.cpp` | NEW — JSON `.luth` format |
| `luth/platform/FileDialog.h/.cpp` | NEW — Win32 native Open/Save dialogs |
| `luth/editor/Editor.h/.cpp` | Scene state, menu bar, shortcuts, scene ops |
| `luth/editor/panels/ProjectPanel.cpp` | Double-click scene loading |
| `luth/editor/panels/HierarchyPanel.cpp` | Removed redundant scene creation |
| `luth/scene/Scene.h/.cpp` | `Clear()` fix, `DestroyEntity` UB fix |
| `luth/core/App.cpp` | Wire `SetActiveScene` |
| `luth/resources/FileSystem.cpp` | Register `.luth` type |
