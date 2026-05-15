# v2.10.2 — jolt-physics-assets

**Date:** 2026-05-15
**Commits:** 7 (on `feat/jolt-physics-assets`)
**Series:** `jolt-physics`, third effort. Mode-B per-effort PATCH bump.

---

## Overview

Phase E of the Jolt integration: asset-backed collision shapes (`JPH::ConvexHullShape` for any motion, `JPH::MeshShape` static-only), an opt-in flag on the Model importer that gates which models the cache will service, and a new `PhysicsMaterial` UUID-keyed asset (friction / restitution / density) wired through the existing `RigidBody.materialUUID` field. Tier 0's `ShapeBuilder` no longer bails on `Collider::ConvexHullRef` and `Collider::MeshRef` — those route through the new `Physics::ShapeCache` for lazy build + reimport invalidation.

The architectural fork that shaped the design: do we bake separate physics blobs into the Model artifact at import time (Unreal's pattern), or reuse the rendering vertex data on demand from the loaded `Model::m_MeshesData`? AAA-engine investigation (Unreal `UBodySetup::CookedFormatData`, Unity `Physics.BakeMesh`, Godot Jolt `JoltShape3D::try_build()`, bevy_rapier `SharedShape`) showed the split runs along engine maturity, not physics backend. Bumping `ModelHeader v3 → v4` for a Tier 0 feature would invalidate every Library artifact in every project; we instead adopted **Godot Jolt's middle path** — runtime per-asset lazy build, in-memory `JPH::ShapeRefC` cache keyed by `(modelUUID, meshIndex, shapeKind)`, sized for our scale. Migration to Unreal-style on-disk cooking later is cheap because the cache key is already content-addressable: a future `feat/jolt-cooked-shapes` effort can persist `JPH::Shape::SaveBinaryState` blobs as a sidecar/appended chunk keyed by the same tuple, with `ShapeCache` gaining a "load-from-blob" branch in front of "build-from-vertices" — no engine API changes.

`Model::m_MeshesData` retention post-upload (Model.cpp:32-73 keeps the vectors live; the Model.h:18-20 doc claiming otherwise is stale) is the green light for option (b). The future on-disk cook lifts that dependency.

---

## Sub-Tasks

| # | What landed | Commits |
|---|---|---|
| A | **ShapeCache scaffold.** `Physics::ShapeCache` member of `PhysicsSystem`. `ShapeKey { u64 hi, lo; u32 meshIndex; u8 kind; }` + FNV-1a hash, SpinLock-guarded `unordered_map`. `BuildOutcome { shape, retryLater }` lets the caller distinguish transient (asset not loaded) from permanent failure. `TryCreateBody` returns a new `BuildResult` enum routed through `DrainPendingBuilds`'s retry shadow vector. Asset-backed bodies pin the source Model via `Scene::HoldAsset` (closes the GC hazard — `RenderSnapshot` mirrored). `AssetDatabase::AddChangeCallback` subscription stages dirty UUIDs into a SpinLock-guarded scratch; `DrainDirtyAssets` runs at the top of `Update`, invalidates matching cache entries, walks the registry to push affected entities. Scene change clears the cache (project switch can recycle UUIDs). | [`57f01d9`](../../../../commit/57f01d9) |
| B | **ConvexHullShape + MeshShape build from `Model::m_MeshesData`.** `ConvexHullShapeSettings` from `Vertex.Position` (or `SkinnedVertex` on skinned meshes — same field offset, different stride); `MeshShapeSettings` from positions + `JPH::IndexedTriangleList` with `% 3 == 0` validation. Per-collider `RotatedTranslatedShape` wrap on non-identity offset/rotation; the cache stores the inner shape so distinct offsets share one inner hull/mesh. | [`ec71729`](../../../../commit/ec71729) |
| C | **`shapeFingerprint` + skip-rebuild fast path.** `ComputeFingerprint(Collider, RigidBody)` hashes Collider type + offset + rotation + active union member + RigidBody motion/layer/sensor/quality/mass. `TryCreateBody` populates `runtime.shapeFingerprint` (was hardcoded `0` and never compared). `DrainPendingBuilds` checks fingerprint match before tear-down → calls new `ApplyRigidBodyTuning` (linearVelocity, angularVelocity, gravityFactor, linearDamping, angularDamping) via `BodyInterface::Set*` and `MotionProperties::Set*Damping` (brief `BodyLockWrite` for the latter). Slider-scrub on damping no longer rebuilds the shape every frame. | [`0bb30f9`](../../../../commit/0bb30f9) |
| D | **`PhysicsMaterial` asset type.** `AssetType::PhysicsMaterial` enum entry (appended), `.physmat` extension + GetTypeInfo entry (orange swatch), `Luth::PhysicsMaterial : public Asset` (friction/restitution/density + `Default()` constexpr fallback), `PhysicsMaterialImporter` mirror of `MaterialImporter`, `AssetSerializer::Serialize/DeserializePhysicsMaterial`, three `AssetManager` dispatch entries (Init + DeserializeArtifact + FinalizeAsset). `luth/assets/physics_materials/Default.physmat` shipped as engine-default reference. | [`9eed75e`](../../../../commit/9eed75e) |
| E | **Apply `PhysicsMaterial` at body creation.** `TryCreateBody` resolves `rb.materialUUID` via `AssetManager::GetAsset<PhysicsMaterial>`; falls back to `PhysicsMaterial::Default()` on miss or invalid UUID. Sets `bcs.mFriction`/`mRestitution`. Density × `shape->GetVolume()` drives mass when `rb.mass <= 0` (skipped on `MeshShape::GetVolume() == 0` — gated to Static where mass is irrelevant). `HoldAsset` pins the material so live bodies aren't undermined by `AssetManager::Trim`. `DrainDirtyAssets` walks the RigidBody view too — material reimport re-queues affected entities. | [`f4ac5ec`](../../../../commit/f4ac5ec) |
| F | **Editor + project surfaces + importer flag.** `PhysicsMaterialEditor` (3 sliders + debounced auto-save mirroring MaterialEditor), wired into `InspectorPanel`. ProjectPanel: `BOWLING_BALL` icon + "Create Physics Material" context-menu entry + `CreateNewPhysicsMaterial` template. ResourcePanel: filter checkbox + Type-color/Type-icon entries. `ModelImportSettings::PhysicsBakeMode { None, Auto }` field + JSON round-trip; `ModelViewer` dropdown surfaces it. `ShapeCache::GetOrBuild` reads `MetaFile::GetTypeSettings` for the model's `physics_bake` flag — on `None` it warns once per UUID and returns null (the entity is dropped from the build queue). | [`addd5ae`](../../../../commit/addd5ae) |
| W | **Wrap-up.** `Version.h` patch bump to v2.10.2. History file. CLAUDE.md Current Progress + Next. BACKLOG.md strike-through Phase E + Tier 1+ deferral list. | [`b5deb56`](../../../../commit/b5deb56) |
| H1 | **Hotfix — rebuild bodies on asset reimport.** Pre-merge audit caught a silent correctness bug in the fingerprint fast path: model + material reimports went through `DrainDirtyAssets` correctly but `DrainPendingBuilds` short-circuited to `ApplyRigidBodyTuning` because no field hashed into `shapeFingerprint` shifts under a content-only change. Fix: `ComputeFingerprint` now hashes `rb.materialUUID` (catches inspector-driven UUID swap) and `DrainDirtyAssets` clears `runtime.shapeFingerprint = 0` for entities it pushes (forces rebuild branch on asset content reimport, same UUID). | [`241514f`](../../../../commit/241514f) |
| H2 | **Hotfix — RigidBody material slot + Default.physmat.meta.** Same audit caught the missing inspector wiring: `RigidBodyDrawer` had no `PropertyAsset` slot for `materialUUID` (couldn't drag-drop a `.physmat`), and OnCopy/OnPaste lambdas dropped the field. Added the slot with `ComponentPropertyCommand<RigidBody, UUID>` + Poke. Also tracked `Default.physmat.meta` so the engine-default UUID stays stable across machines (sibling fonts/shaders convention). | [`e663555`](../../../../commit/e663555) |

---

## Architectural decisions

### Reuse `Model::m_MeshesData` (option b) over baking separate on-disk blobs (option a)

The decision was gated on the user's "research deeply how other engines do it" — same gate as Phase D's Godot-pattern listener. Cross-engine investigation (full report in the `feat/jolt-physics-assets` plan): Unreal bakes `CookedFormatData` per-platform into `UBodySetup` + DDC. Unity cooks lazily on first `MeshCollider` use, in-memory only (well-known stutter spike, no on-disk persistence). Godot 4 editor-bakes a separate `ConcavePolygonShape3D` resource into the scene; the Jolt module then keeps a per-resource lazy `JPH::Shape` cache. bevy_rapier copies into `Arc<dyn Shape>`, no disk artifact, no auto cache.

For Tier 0 (one programmer, ≲ 10K-vert meshes, no streaming/destruction) Godot's middle path fits. The cost of Unreal's pattern is real: bumping `ModelHeader v3 → v4` invalidates every Library artifact in every project for a feature that works fine without it. The migration to (a) is cheap because the `(modelUUID, meshIndex, shapeKind)` cache key is already content-addressable; the future `feat/jolt-cooked-shapes` effort persists `JPH::Shape::SaveBinaryState` blobs keyed by the same tuple and adds a "load-from-blob" branch in front of "build-from-vertices" without changing engine APIs.

### `ShapeCache` is a `PhysicsSystem` member, not a namespace static

Per-instance ownership ties cache lifetime to the `JPH::PhysicsSystem` (no static-init order grief, scene change can `Clear()` cleanly). Mirrors how `LuthContactListener` is a member of `PhysicsSystem` rather than a free-standing global. The cache survives across scenes only where the scene change handler can reach it — explicit `Clear()` on scene swap so a fresh project can't recycle a previous project's UUID and serve a stale shape.

### `ChangeCallback` thread model: stage-then-drain

`AssetDatabase::AddChangeCallback` is parameterless and runs on whatever thread `App::Run` calls `ProcessPendingChanges` from (main loop thread). `PhysicsSystem::Update` runs on the game-stage fiber (potentially on a worker thread). The callback only stages dirty UUIDs into `m_DirtyAssetsScratch` under a SpinLock; the registry walk + cache invalidation + entity re-queue all happen on the game-stage fiber via `DrainDirtyAssets`. Hold time on the lock is a `vector::insert` — well within V1.

### Failed asset-backed builds stay queued (no `AssetManager::OnLoaded` callback)

`AssetManager::GetAsset` returns `nullptr` until upload-queue drain, with no completion notification. `BuildOutcome.retryLater` lets `DrainPendingBuilds` push the entity onto a shadow vector and swap at end-of-loop — effectively retries every Update until the asset arrives. Bounded by physics-entity count, not per-frame work amplification. Cheaper than wiring a new callback through `AssetManager` for one consumer.

### Fingerprint excludes mass/damping/velocity (the tunables)

`ComputeFingerprint` hashes structural fields only: type, offset, rotation, union member, motion, layer, sensor, quality, **mass**. Mass changes force rebuild because Jolt computes inertia from mass + shape at body-create; in-place mass edits would need `MotionProperties::SetMassProperties` + recomputed inertia tensor. Tunables (linearVelocity, angularVelocity, gravityFactor, linearDamping, angularDamping) live outside the fingerprint and route through `ApplyRigidBodyTuning`'s `BodyInterface::Set*` + `MotionProperties::Set*Damping` calls — single-writer outside Step, brief `BodyLockWrite` for the damping setters.

### `PhysicsBakeMode` is a None/Auto gate, not a per-mesh bake destination

The user's Phase E spec mentioned "ModelImporter opt-in shape generation cached on the Model asset" — interpreted as a per-model authorization gate on the cache, not a per-mesh shape-kind selector (which would duplicate the choice already made on `Collider::Type`). The 2-value enum keeps the importer surface minimal. Per-mesh override + `ConvexHullPerMesh`/`MeshShapePerMesh` enums defer to Tier 1 if project data shows they're needed.

---

## Bugs found along the way

| Symptom | Root cause | Fix |
|---|---|---|
| Build error: `usize` undefined in `ShapeCache.h` | The codebase uses `size_t` directly (LuthTypes.h has no `usize` alias). I assumed Rust-style nomenclature. | Replaced `usize` with `size_t` in `ShapeKeyHash::operator()`. |
| Build error: `nlohmann/json_fwd.hpp` not found | The vendored nlohmann/json doesn't ship the forward-declaration header; the codebase uses `<nlohmann/json.hpp>` directly. | Switched include in `PhysicsMaterial.h`. Matches `Material.h` precedent. |
| Build error: `ImTextureID` cannot convert from `nullptr` | `ImTextureID` is `ImU64` (unsigned int) since ImGui v1.91.4, no longer a void*. | Pass `static_cast<ImTextureID>(0)` for the no-thumbnail case in `PhysicsMaterialEditor::Draw`. |
| Asset content reimport (model `.fbx` touch / `.physmat` edit) silently no-op — body kept old shape/material despite the dirty-UUID push reaching `DrainPendingBuilds` | `shapeFingerprint` hashed only Collider + RigidBody field state. Asset content changes don't shift any of those fields, so the fingerprint matched and the fast-path `ApplyRigidBodyTuning` branch ran, skipping shape/material apply | Hotfix H1: include `rb.materialUUID` in the hash + clear `runtime.shapeFingerprint = 0` for entities pushed by `DrainDirtyAssets` |
| `RigidBody.materialUUID` had no inspector control — only assignable by editing the `.luth` JSON; copy-paste lost the field | `RigidBodyDrawer` was authored at Tier 0 before `materialUUID` was meaningful (no `PhysicsMaterial` asset existed); Phase E added the engine wiring + apply path but didn't surface the field to the user | Hotfix H2: `UI::PropertyAsset("Physics Material", ..., AssetType::PhysicsMaterial)` slot + extend OnCopy/OnPaste lambdas |

---

## File list

**New (engine)**
- `luth/source/luth/physics/ShapeCache.h` / `.cpp` — UUID-keyed cache + opt-in gate + once-per-UUID warn set
- `luth/source/luth/physics/PhysicsMaterial.h` / `.cpp` — runtime asset class
- `luth/source/luth/resources/importers/PhysicsMaterialImporter.h` / `.cpp` — JSON copy-validate importer

**New (assets)**
- `luth/assets/physics_materials/Default.physmat` (+ `.meta`) — engine-default reference, stable UUID checked in

**New (editor)**
- `luthien/source/luthien/inspectors/PhysicsMaterialEditor.h` / `.cpp` — three sliders + debounced auto-save

**New (docs)**
- `docs/development/history/v2.x/jolt-physics-assets.md` (this file)

**Modified (engine)**
- `luth/source/luth/scene/systems/PhysicsSystem.h` / `.cpp` — `ShapeCache` member, `BuildResult` enum, retry shadow, ChangeCallback subscription, fingerprint compute + skip-rebuild fast path, `ApplyRigidBodyTuning`, PhysicsMaterial apply
- `luth/source/luth/physics/ShapeBuilder.h` / `.cpp` — comment update; primitives unchanged
- `luth/source/luth/resources/Asset.h` — append `PhysicsMaterial` enum entry
- `luth/source/luth/resources/AssetManager.cpp` — Init importer + 2 dispatch cases
- `luth/source/luth/resources/AssetSerializer.h` / `.cpp` — Serialize/Deserialize PhysicsMaterial
- `luth/source/luth/resources/FileSystem.cpp` — `.physmat` extension + GetTypeInfo entry
- `luth/source/luth/resources/MetaFile.cpp` — empty default-settings case for PhysicsMaterial; `physics_bake: 0` in Model defaults
- `luth/source/luth/resources/importers/ModelImporter.h` / `.cpp` — `PhysicsBakeMode` enum + field + JSON round-trip
- `luth/source/luth/core/Version.h` — patch bump to v2.10.2

**Modified (editor)**
- `luthien/source/luthien/panels/InspectorPanel.h` / `.cpp` — PhysicsMaterialEditor member + dispatch
- `luthien/source/luthien/panels/ProjectPanel.h` / `.cpp` — icon, Create New entry, template JSON
- `luthien/source/luthien/panels/ResourcePanel.h` / `.cpp` — filter checkbox + Type-color/icon entries
- `luthien/source/luthien/inspectors/ModelViewer.cpp` — `Bake Mode` dropdown in Import Settings
- `luthien/source/luthien/inspectors/component_drawers/RigidBodyDrawer.cpp` — `Physics Material` PropertyAsset slot + OnCopy/OnPaste round-trip (hotfix H2)

**Modified (docs)**
- `CLAUDE.md` — Current Progress (latest shipped + next)
- `docs/development/BACKLOG.md` — strike Phase E + Tier 1+ deferrals

---

## Verification

User-authored `samples/assets/scenes/mesh_collide_test.luth` (deferred to user per Phase D's pattern). Suggested contents:

- Static `MeshRef` floor (tessellated plane) + `Default.physmat`
- Two dynamic `ConvexHullRef` Suzannes + `Default.physmat`
- One dynamic Box + custom `bouncy.physmat` (restitution 0.9)

Expected behaviour:
1. Suzannes settle on the mesh surface (not on its AABB).
2. Bouncy box bounces noticeably higher than a default-material box.
3. Edit `bouncy.physmat` → restitution 0.1 mid-Play; within ~1.5s the body rebuilds and stops bouncing.
4. `touch` a Suzanne `.fbx`; cache invalidates and bodies rebuild without crash.
5. `drop_test.luth` and `trigger_test.luth` continue to pass (no regression on primitives or events).

Build verification: Debug + Release x64 clean. Pre-existing `LNK4006` / `C4996 strncpy` / `C4244 chrono` warnings unchanged.

---

## Out of scope

- **On-disk cooked-shape persistence** — future `feat/jolt-cooked-shapes` effort; Unreal-style `JPH::Shape::SaveBinaryState` sidecar/appended chunk keyed by `(modelUUID, meshIndex, shapeKind, content_hash)`. Cheap migration since the cache key is already content-addressable.
- **Per-mesh `PhysicsBakeMode` override** — currently per-model only. The shape kind already comes from `Collider::Type`.
- **`ConvexHullPerMesh` / `MeshShapePerMesh` enum modes** — see above.
- **`JPH::MutableCompoundShape` composition** (multiple Colliders per entity).
- **ConvexDecomposition (V-HACD)** for non-convex shapes.
- **PhysicsMaterial cook parameters** (active-edge angle, double-sided, per-triangle user data) — add when on-disk cook lands.
- **PhysicsMaterial-per-shape** — currently per-body via `RigidBody.materialUUID`.
- **Heightfield shapes** (terrain).
- **Dynamic mesh updates** (skinned mesh as physics, deformable shapes).
- **`JPH::Shape` LOD / decimation.**

---

## Foundation-testing implications

Two new "stack-local handler captures refs to long-lived storage" patterns:
1. The `AssetDatabase::AddChangeCallback` lambda captures `this` for `m_DirtyAssetsLock` + `m_DirtyAssetsScratch`. Lifetime is fine in practice (PhysicsSystem outlives the App loop) but theoretically dangling at shutdown — `AssetDatabase` has no unregister API.
2. The retry-shadow swap in `DrainPendingBuilds` mutates `m_PendingBuild` from the same fiber that's iterating it; the swap-at-end pattern keeps it safe but is a foundation-testing target if the queue ever sees concurrent push from a non-main thread.

Cache invalidation under heavy reimport churn (touching multiple `.fbx` files in rapid succession while bodies are mid-build) is worth a stress-test target. SpinLock contention should stay in the ~µs range; the registry walk in `DrainDirtyAssets` is bounded by entity count not dirty-list size.
