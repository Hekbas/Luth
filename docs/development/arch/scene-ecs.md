# Scene & ECS — Architecture Details

## Overview

EnTT-based ECS with explicit two-way hierarchy, level-based parallel transform propagation, and JSON scene serialization.

## Scene

Owns the `entt::registry` and manages entity lifecycle.

**Key fields:**
- `m_Registry` — EnTT entity/component storage
- `m_RootEntities` — flat vector of parentless entities
- `m_HierarchyVersion` — incremented on structural changes (add/remove/reparent)
- `m_HeldAssets` — `map<UUID, shared_ptr<Asset>>` — prevents GC eviction of in-use assets

**Key methods:**
- `CreateEntity(name)` — auto-adds ID, Tag, Transform, WorldTransform; adds to roots
- `DestroyEntity(entity)` — recursive children-first destruction; updates parent's Children list
- `Clear()` — entity-by-entity destruction (not `registry.clear()`) to preserve EnTT bucket structures
- `DuplicateEntity()` — deep copy of all components + recursive child duplication
- `ReorderEntity()` — drag-drop reordering with reparenting
- `HoldAsset(uuid, asset)` — keeps asset alive; transitively holds Material's texture dependencies

## Entity

Lightweight handle wrapper: `entt::entity` handle + `Scene*` back-reference. Copyable, not owned.

**Component API:** `AddComponent<T>()`, `GetComponent<T>()`, `HasComponent<T>()`, `RemoveComponent<T>()`

**Hierarchy API:** `SetParent()`, `GetParent()`, `GetChildren()`, `IsDescendantOf()`, `IsAncestorOf()`
- Prevents self-parenting and cycles via `IsDescendantOf()` check

## Components (all in `Luth::Component` namespace)

### Core
| Component | Fields |
|-----------|--------|
| **ID** | `UUID m_ID` |
| **Tag** | `string m_Tag` |
| **Parent** | `Entity m_Parent` |
| **Children** | `vector<Entity> m_Children` |

### Transform
| Component | Fields |
|-----------|--------|
| **Transform** | `Position`, `Rotation` (Euler degrees), `Scale`, `LocalMatrix`, `IsDirty` |
| **WorldTransform** | `Matrix` (computed: ParentWorld × LocalMatrix) |

### Rendering
| Component | Fields |
|-----------|--------|
| **MeshRenderer** | `ModelUUID`, `MeshIndex`, `MaterialUUID`, `isSkinned` |
| **DirectionalLight** | `Color`, `Intensity`, `CastShadows`, `ShadowBias` |
| **PointLight** | `Color`, `Intensity`, `Range` |

### Other
| Component | Fields |
|-----------|--------|
| **Camera** | `ProjectionType` (Perspective/Ortho), `FOV`, `Near/Far`, `AspectRatio`, `ViewMatrix`, `ProjectionMatrix`, `IsDirty` |
| **Animation** | `ModelUUID`, `AnimationIndex` |

## Systems

Static `SystemRegistry` class (renamed from `Systems` in arch-cleanup v1.6.0) holds a `vector<unique_ptr<ISystem>>`. Per-system dispatch via `SystemRegistry::Update<T>()` (called explicitly from `App::Run` for each registered system).

**Update order:** TransformSystem → AnimationSystem → RenderingSystem → PickingSystem (camera state fed directly into RenderingSystem from App via `CameraParams`; `LightingSystem` is registered for lookup but its `Update` is a no-op — `RenderingSystem::Update` drives it inline via `UpdateFor`)

### TransformSystem — Parallel Level-Based Hierarchy
1. If hierarchy version changed, rebuild level arrays via BFS from roots
2. Process levels sequentially (level 0 first, then 1, etc.)
3. Within each level, dispatch parallel jobs (group size 64)
4. Each job: recompute `LocalMatrix` if dirty → `WorldTransform = ParentWorld × Local`
5. Parents are guaranteed finalized before children (serial level ordering)

### CameraSystem
- Computes `ViewMatrix = inverse(WorldTransform.Matrix)`
- Recomputes `ProjectionMatrix` from properties only if `IsDirty`

### RenderingSystem (ECS-glue layer since arch-renderer-split v1.7.0; slimmed further in rendering-system-slim v2.6.0)
- ~200 LOC; narrow ECS→DrawList dispatcher. Graph assembly + graphics resources live on `RenderPipeline` in `renderer/`.
- Owns per-frame scene inputs: `FrameTargets`, `CameraParams`, `DrawList`, `FrameDebugger`, editor toggles.
- Calls `LightingSystem::UpdateFor` then `RenderPipeline::Execute` each frame.

### LightingSystem (since rendering-system-slim v2.6.0)
- Owns `LightGatherer` (ECS → `LightUniforms`) and `CascadeBuilder` (PSSM cascade fit).
- `UpdateFor(registry, camera)` is invoked from `RenderingSystem::Update` (its `ISystem::Update` is a no-op); `RenderingSystem` then hands the outputs to `RenderPipeline::UploadLightUBO` + `UpdateGlobalUniforms`.

### PickingSystem (since rendering-system-slim v2.6.0)
- Owns the single-pixel Vulkan readback from the `EntityID` target + the `RequestPick`/`HasResult`/`ConsumeResult` state.
- Registered last in the update order so the `EntityID` target has valid contents when it reads.
- Editor panels reach it via `SystemRegistry::GetSystem<PickingSystem>()`.

- Detailed rendering architecture in `arch/rendering-pipeline.md`.

## Scene Serialization (JSON `.luth` format)

**Save:** Depth-first traversal (parents before children). Serializes UUID, Tag, active state, parent UUID, Transform, and all optional components.

**Load:** Two-pass — (1) create all entities with components, (2) reconstruct hierarchy from stored parent UUIDs.

**Hierarchy preservation:** DFS order ensures parent exists in UUID lookup map before child references it.
