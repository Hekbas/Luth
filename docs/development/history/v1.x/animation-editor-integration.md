# Phase 7E — Editor Integration

**Date:** 2026-03-29

---

## Overview

Editor integration for the animation system: full inspector UI for Animation and BoneAttachment components, automatic model instantiation with bone hierarchy entities, bone debug visualization overlay, and multiple runtime bugfixes discovered during testing.

---

## Animation Inspector

**File:** `editor/panels/InspectorPanel.cpp`

Full `DrawComponent<Animation>` implementation replacing the previous placeholder:

- **Model auto-sync:** If `ModelUUID` is unset but the entity has a MeshRenderer, copies the UUID automatically
- **Clip selector:** Combo populated from `Model::GetAnimationClips()`, writes to `Animation::AnimationIndex`
- **Transport controls:** Play (FA_PLAY), Pause (FA_PAUSE), Stop (FA_STOP) buttons
- **Speed slider:** Range 0.0–5.0, controls `Animation::Speed`
- **Loop checkbox:** Toggles `Animation::Loop`
- **Timeline scrubber:** SliderFloat from 0 to clip duration in seconds, also sets `CurrentTime` when paused for manual scrubbing
- **Frame counter:** Displays current frame as `time * ticksPerSecond`
- **Show Bones toggle:** Checkbox bound to `EditorSettings::showBoneDebug`

---

## BoneAttachment Inspector

**File:** `editor/panels/InspectorPanel.cpp`

- **Target entity combo:** Iterates `registry.view<Animation, Tag>()` to list eligible targets
- **Bone name dropdown:** Populated from target entity's model skeleton
- **LocalOffset / LocalRotation:** Vec3 property editors for bone-space offsets

Added "Bone Attachment" to the Add Component popup menu.

---

## Model Instantiation with Bone Hierarchy

**File:** `editor/panels/HierarchyPanel.cpp` — `InstantiateModel()`

When instantiating a skinned model:

1. Creates root entity with `Animation(assetUuid)` component
2. Creates child entities per mesh with `MeshRenderer` (existing behavior)
3. Creates bone hierarchy entities mirroring skeleton structure

```
Model (Animation comp)
├── Mesh1 (MeshRenderer comp)
├── Mesh2 (MeshRenderer comp)
└── RootBone
    ├── ChildBone1
    │   └── GrandchildBone
    └── ChildBone2
```

Bones are topologically sorted (parent before child), so `boneEntities[bone.ParentIndex]` is always already created. Bone entities are structural only — Tag + Transform + WorldTransform, no MeshRenderer or Animation.

---

## Bone Debug Overlay

**Files:** `editor/panels/ScenePanel.cpp/.h`, `editor/EditorSettings.cpp/.h`

`DrawBoneDebugOverlay()` renders skeleton wireframe in the scene viewport:

1. Finds Animation component on selected entity or its parent
2. Reads `GlobalBoneTransforms` from Animation component
3. Projects 3D bone positions to 2D via View-Projection matrix
4. Draws lines between parent-child bones using `ImDrawList`

Controlled by `EditorSettings::showBoneDebug` with JSON persistence.

---

## ModelViewer Enhancement

**File:** `editor/inspectors/ModelViewer.cpp`

Animation table expanded from 3 to 5 columns: Name, Duration (ticks), Duration (s), TPS, Events.

---

## Runtime Bugfixes

### AnimationSystem view query

AnimationSystem previously required `<Animation, MeshRenderer, WorldTransform>`. Since the root entity now has Animation but no MeshRenderer (meshes are on children), the view was changed to `<Animation, WorldTransform>`. AABB computation uses a fallback `MeshIndex = 0` when MeshRenderer is absent.

### RenderingSystem parent traversal

Both shadow pass and geometry pass now traverse the parent entity to find the Animation component. When a MeshRenderer child is being drawn, the system checks if its Parent entity has Animation and reads `BoneBufferOffset` from there.

### Assimp FBX intermediate nodes

`ModelImporter::ExtractSkeleton()` was missing Assimp's FBX decomposition nodes (`$AssimpFbx$_Translation`, `$AssimpFbx$_PreRotation`, etc.). Animation channels target these nodes, but they weren't included in the skeleton, causing `FindBone()` to return -1 and silently dropping ALL animation tracks.

Fixed by marking nodes targeted by animation channels (and their ancestors) as relevant during skeleton extraction. Added a warning log for unmatched channels.

### SceneSerializer Playing state

`SceneSerializer` hardcoded `aj["playing"] = false`, causing animations to always deserialize as paused regardless of runtime state. Changed to save the actual `Animation::Playing` value.

### Entity.h accessor

Added `Entity::GetScene()` public accessor for BoneAttachment inspector to iterate the scene registry.

---

## Files Modified

| File | Change |
|------|--------|
| `editor/panels/InspectorPanel.cpp` | Animation inspector, BoneAttachment inspector, Add Component entries |
| `editor/panels/HierarchyPanel.cpp` | InstantiateModel: Animation auto-add + bone hierarchy entities |
| `editor/panels/ScenePanel.cpp/.h` | Bone debug overlay |
| `editor/EditorSettings.cpp/.h` | `showBoneDebug` setting with JSON persistence |
| `editor/inspectors/ModelViewer.cpp` | Animation table (5 columns) |
| `scene/Entity.h` | `GetScene()` accessor |
| `scene/systems/AnimationSystem.cpp` | View query fix, diagnostic logging |
| `scene/systems/RenderingSystem.cpp` | Parent traversal for Animation component |
| `resources/importers/ModelImporter.cpp` | $AssimpFbx$ intermediate node inclusion |
| `scene/SceneSerializer.cpp` | Save actual Playing state |
| `core/App.cpp` | AnimationSystem registration |
