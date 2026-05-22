# Luth Engine — Future Ideas

Long-tail wishlist of unscheduled work, grouped by area. Nothing here has a
target version or scheduled slot — items move to [`ROADMAP.md`](ROADMAP.md)
when they become "next." Pulled here when planning a series so we don't lose
the seed.

For what's done or planned next, see [`ROADMAP.md`](ROADMAP.md).
For per-system architecture references, see [`arch/`](arch/).
For shipped history, see [`history/`](history/).

---

## Physics maturity

Carried out of the `jolt-physics` v2.10.x series. Land when project data
demands them.

| Item | Notes |
|---|---|
| **`feat/jolt-cooked-shapes`** | On-disk persistence via `JPH::Shape::SaveBinaryState` sidecar/appended chunk keyed by `(modelUUID, meshIndex, shapeKind)`. `Physics::ShapeCache` key is already content-addressable, so the migration is a "load-from-blob" branch with no engine API changes. Trigger: startup time / RAM from in-memory shape rebuild becomes a complaint. |
| **`jolt-character-tier-2`** | `CharacterContactListener`, dynamic-body push, crouch, swim. Trigger: first project that needs character ↔ dynamic-body interaction. |
| **Per-mesh `PhysicsBakeMode` override** | Currently per-model. Add `ConvexHullPerMesh` / `MeshShapePerMesh` enum modes when authoring asks for it. |
| **`JPH::MutableCompoundShape` composition** | Multiple Colliders per entity / child-entity composition. Trigger: first model needing multi-part collision (vehicle chassis, articulated prop). |
| **ConvexDecomposition (V-HACD)** | Non-convex dynamic shapes that can't be a single ConvexHull. Trigger: first non-convex dynamic object that can't be hand-decomposed. |
| **`PhysicsMaterial` cook parameters** | Active-edge angle, double-sided, per-triangle user data. Pairs with on-disk cook landing. |
| **`PhysicsMaterial`-per-shape** | Currently per-body via `RigidBody.materialUUID`; per-shape is the natural next refinement (different friction on different sides of one body, character-material slot). |
| **Heightfield / terrain shapes** | `JPH::HeightFieldShape` for streamed terrain. Trigger: terrain system arrives. |
| **Dynamic mesh updates** | Skinned mesh as physics, deformable shapes. Trigger: cloth / soft-body system arrives. |
| **`JPH::Shape` LOD / decimation** | Distance-based collider LODs. Trigger: profiler flags it. |

## Gameplay enablement

| Item | Depends on | Notes |
|---|---|---|
| **Scripting (C# via Mono, or Lua)** | `play-mode` ✅, `jolt-physics` ✅ | Top priority of this category — deletes the `PlayerControllerSystem` stub. Big design surface (language pick, embedding, hot-reload, ECS bindings). |
| **Prefab System** | `play-mode` ✅, scripting (recommended) | Entity templates with override tracking. |
| **Ragdoll** | `jolt-physics` ✅, `animation-controller-v2` | Bone driven by Jolt body during ragdoll state. |

## Rendering (beyond planned-epic deps)

| Item | Depends on | Notes |
|---|---|---|
| **Full deferred shading** | `rt-renderer` (slim G-buffer lands in Phase A.2) | If the forward path hits material-variant complexity limits; the slim G-buffer is already there feeding RT denoising, motion vectors, and SSR-replaced-by-RT-reflections. Full deferred would extend it with albedo + metallic/roughness + emissive + opaque shading model ID. |
| **HZB Occlusion Culling** | `compute-gpu-culling` ✅, `gpu-driven` series | Two-phase cull pipeline; depth pyramid as compute. Lands as part of the post-`rt-renderer` `gpu-driven` series (mesh shaders + GPU-driven culling). |
| **Decals (screen-space deferred)** | `rt-renderer` slim G-buffer | Cluster-binned (same pattern as lights). Could land late in `rt-renderer` arc or as polish; not on the critical path for Bhaal Temple. |

> Removed because absorbed into `rt-renderer`: Volumetric Fog → Phase A.4 (Wronski full voxel volume), Global Illumination → Phase C (ReSTIR DI + GI), SSR → Phase D.1 (RT reflections supersede screen-space). Shadow frustum-union fit removed because Phase B.3 retires CSM entirely.

## Animation maturity (post `animation-controller-v2`)

| Item | Depends on | Notes |
|---|---|---|
| **Animation v3 (DQS, IK, morph targets)** | `animation-controller-v2` | Beyond state machine + blend trees. |

## Audio

| Item | Depends on | Notes |
|---|---|---|
| **3D Spatial Audio** | `play-mode` ✅ | Orthogonal; integrate when demo needs it. |
| **Audio asset pipeline** | — | `.wav` / `.ogg` importer + AssetManager dispatch. |

## Editor & Tools

| Item | Depends on | Notes |
|---|---|---|
| **Asset Streaming** | `compute-gpu-culling` ✅ | Async GPU upload via transfer queue. |
| **Visual Shader Editor** | — | Editor luxury, low priority. |
| **Frame Debugger per-view capture** | `game-panel` ✅ | Capture currently scene-view-only; extend tracked RTs + archive slots to tag by view. |
| **Scene-panel post-process toggle** | — | Unity-style toggle to disable bloom/tonemap/vignette for lookdev. |
| **Auto-wire `GPUTimerPool` into RenderGraph** | — | Currently per-pass insertion is manual. |

## Profiling / Memory

| Item | Depends on | Notes |
|---|---|---|
| **Tracked STL allocators (`LH::Vector<T>`)** | — | Closes the STL gap on `MemoryTracker` (currently covered only by Tracy hooks). |
| **GPU heap stress test** | `foundation-testing` ✅ | `GPUTaggedPageAllocator` V6 overflow + backing-pool growth under sustained pressure. Deferred from foundation-testing because of Vulkan-device setup overhead. CPU side covers the architectural shape. |
