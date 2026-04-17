# Phase 5-H: Inspector + Material Editor Completion ✅ (2026-03-16)

Four editor improvements: MeshRenderer/Animation in Add Component dropdown, DirectionalLight shadow controls, albedo color picker, and material Save button with dirty tracking.

### Task 1 — Add MeshRenderer & Animation to "Add Component" Dropdown
**File:** `InspectorPanel.cpp`

Added `MeshRenderer` and `Animation` entries to the "Add Component" popup, after the existing `PointLight` entry. Both check `HasComponent<T>()` before showing.

### Task 2 — DirectionalLight Shadow Controls
**Files:** `Components.h`, `InspectorPanel.cpp`, `RenderingSystem.h/.cpp`, `pbr.frag`, `pbr.vert`, `shadowDepth.vert`

Added `CastShadows` (bool) and `ShadowBias` (float) to `DirectionalLight` component. Inspector shows checkbox + drag slider (0.0–0.05). `RenderingSystem` reads these fields and uploads `shadowBias` in `GlobalUniforms`. Negative bias (-1.0) signals shadows disabled in the shader — `ComputeShadow()` returns 1.0 immediately. All shaders sharing Set 0 binding 0 updated to match the new UBO layout.

### Task 3 — Albedo Color Picker
**Files:** `Material.h`, `Material.cpp`, `InspectorPanel.cpp`

Added `SetColor()`/`GetColor()` to Material for direct `GPUMaterialData.color` access (bypasses uniform storage since SPIRV-Cross is disabled). `UpdateGPUData()` updated to only overwrite color from uniform storage if the uniform exists, otherwise preserving the `SetColor()` value. Color picker widget placed between shader selector and properties section. Color serialized/deserialized in material JSON.

### Task 4 — Material Save Button + Dirty Indicator
**Files:** `Material.h`, `InspectorPanel.cpp`

Added `m_Dirty` flag with `IsDirty()`/`MarkDirty()`/`ClearDirty()`. Inspector shows `*` in material header when dirty. "Save" button serializes material JSON to disk via `AssetSerializer::SerializeMaterial()`. All material edits (render mode, alpha cutoff, blend factors, texture changes, color, shader) call `MarkDirty()`.

### Files Modified

| File | Changes |
|---|---|
| `luth/scene/Components.h` | `CastShadows`, `ShadowBias` fields on `DirectionalLight` |
| `luth/renderer/Material.h` | `SetColor()`/`GetColor()`, dirty flag (`m_Dirty`, `IsDirty`, `MarkDirty`, `ClearDirty`) |
| `luth/renderer/Material.cpp` | Color serialization, conditional uniform→GPUData color sync |
| `luth/editor/panels/InspectorPanel.cpp` | All 4 tasks: Add Component entries, DirLight UI, color picker, Save button + dirty tracking |
| `luth/scene/systems/RenderingSystem.h` | `shadowBias` + `_pad[3]` in `GlobalUniforms`, cached shadow fields |
| `luth/scene/systems/RenderingSystem.cpp` | Read shadow fields from component, upload bias, conditional shadow disable |
| `sandbox/assets/shaders/pbr.frag` | `shadowBias` in UBO, bias application in `ComputeShadow()` |
| `sandbox/assets/shaders/pbr.vert` | `shadowBias` + `_pad[3]` in GlobalUniforms (UBO layout match) |
| `sandbox/assets/shaders/shadowDepth.vert` | `shadowBias` + `_pad[3]` in GlobalUniforms (UBO layout match) |
