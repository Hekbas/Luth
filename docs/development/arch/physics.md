# Physics Architecture

## Overview

Jolt Physics 5.5.0 wrapped behind a single ECS-driven `PhysicsSystem`. Bodies are constructed lazily from `(Collider, RigidBody, Transform)` triples on the active scene; characters from `(Collider Type::Capsule, CharacterController, Transform)`. Jolt's parallel work routes through Luth's fiber scheduler via a custom `JPH::JobSystem` adapter — no semaphore-blocking kernel waits.

Four design constraints shape the whole subsystem:

1. **EnTT signal-driven lifecycle.** Body/character creation defers to the start of `PhysicsSystem::Update` via `m_PendingBuild`. `on_construct` fires synchronously inside `AddComponent` before the caller can populate fields; queueing closes that race and dedups multi-edit churn (slider scrubs rebuild once per frame, not per pixel).
2. **Fixed-dt accumulator.** `kFixedDt = 1/60`, up to `kMaxSubSteps = 4` per render frame. Deterministic stepping that survives frame stalls without spiral-of-death.
3. **Query re-entry guard.** `m_StepInFlight` atomic asserts on any `Raycast` / `Overlap` called from inside `JPH::PhysicsSystem::Update` (including listener callbacks). Jolt's `NarrowPhaseQuery` holds body locks during broadphase mutation — re-entry would deadlock.
4. **Fingerprint fast path.** `DrainPendingBuilds` checks a content hash of structural fields before tear-down; matching fingerprints route through `Apply*Tuning` (in-place setters) instead of rebuild.

`arch/physics.md` is the reference doc. Per-effort archaeology lives in `history/v2.x/rigid-bodies.md` (v2.10.0), `jolt-physics-queries.md` (v2.10.1), `jolt-physics-assets.md` (v2.10.2), `jolt-character-controller.md` (v2.10.3).

---

## Components

All in `Luth::Component` namespace, declared in `luth/source/luth/scene/components/Physics.h`.

### Authoring

| Component | Fields | Notes |
|---|---|---|
| **Collider** | `type: {Box, Sphere, Capsule, ConvexHullRef, MeshRef}`, `localOffset`, `localRotation`, union: `boxHalfExtents` / `sphereRadius` / `capsule{radius, halfHeight}` / `meshRef{modelHi, modelLo, meshIndex}` | Tagged union, `static_assert(sizeof <= 64)`. Mesh/Convex hold a UUID into a `Model` asset. |
| **RigidBody** | `motion: {Static, Kinematic, Dynamic}`, `motionQuality: {Discrete, LinearCast}`, `layer: u8`, `isSensor: bool`, `startActive: bool`, `mass: f32`, `linearVelocity`, `angularVelocity`, `gravityFactor`, `linearDamping`, `angularDamping`, `materialUUID` | `mass <= 0` derives from `density * shape->GetVolume()` at body build. `materialUUID` invalid → `PhysicsMaterial::Default()`. |
| **CharacterController** | `maxSlopeAngleDeg`, `mass=70`, `maxStrength=100`, `characterPadding=0.02`, `predictiveContactDistance=0.1`, `penetrationRecoverySpeed=1.0`, `layer=1` (MOVING), `gravityFactor`, `moveSpeed=5`, `jumpSpeed=6`, runtime `desiredVelocity`, `jumpQueued`, read-back `groundState`, `currentVelocity` | Requires a paired `Collider Type::Capsule`. Inline API: `SetDesiredVelocity(Vec3)` / `Jump()` / `IsGrounded() const`. |

### Runtime (NOT serialized — `PhysicsSystem` writes these)

| Component | Fields | Owner |
|---|---|---|
| **PhysicsBodyRuntime** | `bodyId: u32` (opaque `JPH::BodyID`), `shapeFingerprint: u64` | Body lives in `JPH::PhysicsSystem`; runtime is an opaque handle. |
| **CharacterControllerRuntime** | `character: JPH::CharacterVirtual*` (non-owning), `fingerprint: u64` | Heap allocation owned by `PhysicsSystem::m_CharacterMap`. |
| **GroundState** (enum) | `OnGround / OnSteepGround / NotSupported / InAir` | Mirrors `JPH::CharacterBase::EGroundState`. |

### Mutual exclusion

An entity may have RigidBody OR CharacterController, never both. `DrainPendingBuilds` warns once and drops the entity if both are present.

---

## PhysicsSystem

Single ECS system, in `luth/source/luth/scene/systems/PhysicsSystem.{h,cpp}`. Owns the JPH world and all auxiliary state.

### Owned types

```
PhysicsSystem
├── m_System                : JPH::PhysicsSystem        — the Jolt world
├── m_TempAlloc             : JPH::TempAllocatorImpl    — 32 MB scratch
├── m_JobAdapter            : LuthJobSystemForJolt      — Jolt → fiber scheduler
├── m_BPLayers / m_OvBpFilter / m_LayerPairFilter       — see "Layers" below
├── m_ContactListener       : LuthContactListener       — see "Contact listener" below
├── m_Queue                 : MPMCQueue<PhysicsEvent, 4096>
├── m_ShapeCache            : Physics::ShapeCache       — see "Asset-backed shapes"
│
├── m_BodyMap               : unordered_map<entt::entity, JPH::BodyID>
├── m_CharacterMap          : unordered_map<entt::entity, JPH::CharacterVirtual*>
├── m_EntityByBodyIndex     : vector<entt::entity>(kMaxBodies) — reverse lookup for OnContactRemoved
│
├── m_PendingBuild          : vector<entt::entity>      — deferred lifecycle queue
├── m_PendingDestroy        : vector<{entity, BodyID}>
├── m_PendingCharacterDestroy : vector<{entity, CharacterVirtual*}>
│
├── m_StepInFlight          : atomic<bool>              — query re-entry guard
├── m_Accumulator           : f32                       — fixed-dt accumulator
└── m_DirtyAssetsScratch    : vector<UUID> (SpinLock-guarded) — hot-reload staging
```

Constants: `kMaxBodies = 16384`, `kMaxBodyPairs = 65536`, `kMaxContactConstraints = 16384`, `kNumBodyMutexes = 0` (single-writer outside Step), `kFixedDt = 1/60`, `kMaxSubSteps = 4`.

### Per-frame update order

```
PhysicsSystem::Update(scene)
├── EnsureSignalsConnected      — attach EnTT signals on first scene; scene-change teardown
├── EnsureChangeCallbackRegistered — AssetDatabase reimport hook (once)
├── DrainDirtyAssets             — apply reimport invalidation to fingerprints + queue
├── DrainPendingDestroys         — remove bodies; clear m_EntityByBodyIndex slots
├── DrainPendingCharacterDestroys — delete CharacterVirtual ptrs
├── DrainPendingBuilds           — RB-vs-CC dispatch; fingerprint fast-path or rebuild
├── DrawDebugBodies              — runs regardless of PlayState (authoring visibility)
│
├── if (PlayState == Editing) return; m_Accumulator = 0
│
├── m_Accumulator += Time::DeltaTime(); clamp to kMaxSubSteps * kFixedDt
├── SyncTransformsToBodies       — Kinematic pre-step: MoveKinematic(target, dt)
│
├── for substep in 0..substeps:
│     m_StepInFlight = true
│     UpdateCharacters(kFixedDt) — character sweeps (see "Character integration")
│     m_System.Update(kFixedDt, 1, &m_TempAlloc, &m_JobAdapter)
│     m_StepInFlight = false
│
├── SyncBodiesToTransforms       — Dynamic post-step: write pose back; const_cast velocity update
└── SyncCharactersToTransforms   — write character position back; flip Transform.IsDirty
```

The guard wraps **both** `UpdateCharacters` and `m_System.Update` — character sweeps hold body locks too. Extracted from `Step()` to the loop body in v2.10.3 for this reason.

---

## Body lifecycle

Signal-driven. EnTT `on_construct/update/destroy` for both `Collider` and `RigidBody` route through `OnComponentConstructed/Updated/Destroyed`. Construct + Update push onto `m_PendingBuild`; Destroy queues both `DestroyBodyForEntity` and `DestroyCharacterForEntity` (each `m_*Map.find()` early-returns on miss — no discriminator needed).

### `BuildResult` dispatch

```
enum class BuildResult { Created, RetryLater, Failed };
```

Per entity in `DrainPendingBuilds`:

| Entity has | Path | Failure modes |
|---|---|---|
| RB + CC (both) | Warn-once, drop from queue | — |
| RB | Body path: fingerprint match → `ApplyRigidBodyTuning`; mismatch → destroy + `TryCreateBody` | `MeshRef + non-Static` → Failed. Asset not loaded → RetryLater (shadow-vector retry). |
| CC | Character path: fingerprint match → `ApplyCharacterTuning`; mismatch → `delete` + `TryCreateCharacter` | Non-Capsule Collider → warn-once + Failed. |
| Neither | Drop from queue (Failed at TryCreateBody) | — |

`RetryLater` swaps into `m_PendingBuild` at end-of-loop; effectively retries every Update until the asset arrives. Bounded by physics-entity count, not work amplification.

### Fingerprint inputs

| Path | `ComputeFingerprint` hashes | Tunables (NOT hashed → fast path) |
|---|---|---|
| Body (`ShapeCache::ComputeFingerprint`) | Collider type, offset, rotation, active union member; RigidBody motion, layer, sensor, motionQuality, mass, **materialUUID** (v2.10.2 H1) | linearVelocity, angularVelocity, gravityFactor, linearDamping, angularDamping |
| Character (`ComputeCharacterFingerprint`) | Capsule radius+halfHeight, Collider offset+rotation, characterPadding, predictiveContactDistance | mass, maxStrength, maxSlope, penetrationRecoverySpeed (have JPH setters) |

Mass is structural for bodies (Jolt computes inertia from mass + shape at body-create) but tunable for characters (no inertia tensor).

### Hot-reload

`AssetDatabase::AddChangeCallback` (runs on App-loop thread) → push UUIDs into `m_DirtyAssetsScratch` under `SpinLock`. `DrainDirtyAssets` (game-stage fiber, start of Update) snapshots, invalidates `m_ShapeCache` entries, walks `Collider` + `RigidBody` views to push affected entities. Also zeroes `runtime.shapeFingerprint` so the fast path can't short-circuit on a content-only change with the same UUID.

---

## Character integration

`JPH::CharacterVirtual` is **not** a JPH body — it's a query-driven swept capsule. `PhysicsSystem::m_CharacterMap` owns the heap-allocated pointers (Jolt-internal allocator via `JPH_OVERRIDE_NEW_DELETE` on `CharacterVirtualSettings` — documented exception to cornerstone #4).

### `UpdateCharacters` per substep

```cpp
const Vec3 worldGravity = Physics::FromJolt(m_System.GetGravity());
for each (CharacterController cc, CharacterControllerRuntime runtime) {
    // Compose velocity. JPH does NOT integrate gravity — caller's responsibility (CharacterVirtual.h:322).
    Vec3 v;
    v.x = cc.desiredVelocity.x;
    v.z = cc.desiredVelocity.z;
    v.y = cc.currentVelocity.y + worldGravity.y * cc.gravityFactor * dt;
    if (cc.jumpQueued && cc.IsGrounded()) v.y = cc.jumpSpeed;
    cc.jumpQueued = false;  // consume regardless (lost jump if not grounded — intentional)

    runtime.character->SetLinearVelocity(Physics::ToJolt(v));
    runtime.character->ExtendedUpdate(dt, Physics::ToJolt(worldGravity * cc.gravityFactor),
                                       ExtendedUpdateSettings{},  // defaults: StickToFloor + WalkStairs
                                       BroadPhaseLayerFilter{}, ObjectLayerFilter{},
                                       BodyFilter{}, ShapeFilter{}, m_TempAlloc);

    cc.currentVelocity = Physics::FromJolt(runtime.character->GetLinearVelocity());
    cc.groundState     = ToGroundState(runtime.character->GetGroundState());
}
```

`ExtendedUpdate` (not plain `Update`) gives default stair-walking + stick-to-floor — those are the "JPH defaults" the Tier 1 spec promised. Empty filters mean the character collides per the existing `BPLayerInterface` rules (MOVING vs STATIC/TRIGGER) with no extra filtering at Tier 1. Setting `m_TempAlloc` reuses PhysicsSystem's scratch (frees on Update return).

### Pose sync

`SyncCharactersToTransforms` writes `ch->GetPosition()` into `Transform.Position`, flips `IsDirty`, and updates `WorldTransform.Matrix` directly (TransformSystem reads this next frame). Character rotation is **not** driven by the sweep — user-authored `Transform.Rotation` is preserved.

---

## Layers

Three object layers in `luth/source/luth/physics/PhysicsLayers.h`:

| Object layer | Body kinds | Broadphase tree |
|---|---|---|
| `STATIC` (0) | Static geometry, kinematic ground | STATIC |
| `MOVING` (1) | Dynamic rigid bodies, **the character controller** | MOVING |
| `TRIGGER` (2) | Static sensor zones | TRIGGER |

Pairwise collision (`ObjectLayerPairFilterImpl`): STATIC vs MOVING ✓; MOVING vs everything ✓; TRIGGER vs MOVING only.

Dynamic sensors stay on MOVING with `RigidBody::isSensor = true` — the layer split is for broadphase pruning, not sensor flag storage.

### Query filtering

`Physics::LayerMaskFilter` (`luth/source/luth/physics/LayerMaskFilter.h`) wraps a `u32` bitmask of `1u << Layers::X` into Jolt's `BroadPhaseLayerFilter` + `ObjectLayerFilter` interfaces. Pass `0` to skip layer filtering.

---

## Asset-backed shapes (`Physics::ShapeCache`)

`luth/source/luth/physics/ShapeCache.{h,cpp}` — per-`PhysicsSystem` member (mirrors how `LuthContactListener` is owned), SpinLock-guarded `unordered_map`.

### Key

`ShapeKey { u64 modelHi, modelLo; u32 meshIdx; u8 kind }` — content-addressable, hashed via FNV-1a. The cache key is already migration-ready for a future on-disk cook (`feat/jolt-cooked-shapes` would load `JPH::Shape::SaveBinaryState` blobs keyed by the same tuple).

### Build outcome

```
BuildOutcome { shape: JPH::RefConst<JPH::Shape>, retryLater: bool }
```

`shape == nullptr` means no body this frame. `retryLater` discriminates transient miss (asset not loaded) from permanent failure (bad params, opt-out, invalid UUID). Primitives skip the map entirely — they delegate to `ShapeBuilder` inline.

### Asset path

`ConvexHullShape` from `Vertex.Position` (or `SkinnedVertex.Position` for skinned meshes — same field offset, different stride). `MeshShape` from positions + `JPH::IndexedTriangleList`, with `% 3 == 0` validation. Per-collider `RotatedTranslatedShape` wrap on non-identity offset/rotation; the cache stores the inner shape so distinct offsets share one inner hull/mesh.

### Opt-in gate

`ModelImportSettings::PhysicsBakeMode { None, Auto }` field on the source `Model`. `None` → `GetOrBuild` warns once per UUID and returns null. Authoring choice, not a runtime decision.

### Lifetime

`Scene::HoldAsset` pins the source `Model` while a body holds the shape — closes the AssetManager-Trim GC hazard. Same for `PhysicsMaterial`. Scene change clears the cache (project switch can recycle UUIDs).

---

## PhysicsMaterial asset

UUID-keyed asset class (`luth/source/luth/physics/PhysicsMaterial.{h,cpp}`). Three fields: `friction`, `restitution`, `density`. `Default()` constexpr fallback for invalid UUIDs. Source `.physmat` JSON, binary artifact via standard `AssetSerializer` path, Inspector via `PhysicsMaterialEditor` (three sliders + debounced auto-save).

Currently per-body via `RigidBody.materialUUID`. Future migration is per-shape on `Collider` (see BACKLOG); the character entity will inherit the material via its Collider at that point.

---

## Contact listener + events

`luth/source/luth/physics/PhysicsListeners.{h,cpp}`. Godot-pattern: cross-frame trigger-pair cache (`unordered_set<u64>` of packed BodyID pairs) under SpinLock, populated from both `OnContactAdded` AND `OnContactPersisted` (so runtime `Body::SetIsSensor` flips emit matching Enter/Exit), looked up + cleared in `OnContactRemoved`.

### Event surface

```cpp
enum class PhysicsEventType : u8 { ContactAdded, ContactRemoved, TriggerEnter, TriggerExit };
struct PhysicsEvent { type, entityA, entityB, normal, point };  // 40 bytes
```

Listener writes to `MPMCQueue<PhysicsEvent, 4096>`. Gameplay drains via `PhysicsSystem::DrainEvents(std::span<PhysicsEvent>)` once per frame. Overflow drops the event and bumps an atomic counter (warning logged once per frame in `DrainEvents`).

### `OnContactRemoved` constraint

Jolt forbids body access in `OnContactRemoved` (the body may already be destroyed). Resolution: `m_EntityByBodyIndex` is a `vector<entt::entity>(kMaxBodies)` reverse table, indexed by `BodyID.GetIndex()`. Populated at `TryCreateBody` alongside `SetUserData`; cleared at body destroy. Listener carries a `std::span<entt::entity>` view; lock-free reads (table only mutates outside Step).

### Pair key

`u64 = (max(GetIndexAndSequenceNumber(a, b)) << 32) | min(...)`. Sorted ascending so the key is order-independent. The 8-bit sequence number in `BodyID` prevents slot-reuse collisions at realistic churn rates.

---

## Queries

`PhysicsSystem::Raycast` + `OverlapBox` / `OverlapSphere` / `OverlapCapsule` in `luth/source/luth/scene/systems/PhysicsSystem.h`. All routed through `JPH::NarrowPhaseQuery`.

| API | Result | Filter |
|---|---|---|
| `Raycast(origin, dir, maxDist, layerMask, RaycastHit&) → bool` | Out hit: entity, bodyId, fraction, distance, point, normal | `u32` layerMask |
| `OverlapBox(center, halfExtents, rot, layerMask, std::span<OverlapHit>) → u32` | Count written (clamped to span size) | u32 layerMask |
| `OverlapSphere(center, radius, …)` / `OverlapCapsule(center, radius, halfHeight, rot, …)` | Same | Same |

**Re-entry guard.** Every query asserts `!m_StepInFlight.load(acquire)` first. Calling from inside a contact callback would deadlock on body locks. World normals for raycast hits computed post-call via `BodyLockRead` + `Body::GetWorldSpaceSurfaceNormal` (Jolt's `RayCastResult` lacks normal data).

---

## Debug rendering

`DrawDebugBodies` (`PhysicsSystem::Update`, runs regardless of PlayState). Walks the EnTT `(Collider, WorldTransform, RigidBody, PhysicsBodyRuntime)` view + separate `(CharacterController, …, CharacterControllerRuntime)` view. Emits wire primitives via `Luth::DebugDraw`:

- Sphere: 3 orthogonal great circles (Unity-style, ~30× cheaper than Jolt's tessellated `DebugRendererSimple` path).
- Box: 12 edges from local ±halfExtents.
- Capsule: 2 equator circles + 4 axial seams + 4 hemisphere arcs.
- Character capsule: same as Capsule, color reflects `GroundState` (green/yellow/red/grey).

Three independent passes (Shapes / AABBs / CoM), each with Selected/All scopes. Color modes (`PhysicsDebugColorMode` in `EditorHooks.h`): `Uniform`, `ByMotionType`, `BySleepState` (mirrors Jolt's `EShapeColor::SleepColor` — static grey, kinematic green, dynamic-active yellow, sleeping red). Sensor bodies render at half alpha. Skipped entirely when no editor hook is registered (runtime build).

---

## Job system integration

`luth/source/luth/physics/LuthJobSystemForJolt.{h,cpp}` implements `JPH::JobSystem` directly (NOT `JobSystemWithBarrier` — its semaphore would block worker fibers on a kernel wait and bypass V5 entirely).

| Jolt concept | Luth mapping |
|---|---|
| `Job::Execute()` | Trampoline runs via `Luth::JobSystem::Execute(TrampolineFn, job, /*counter*/nullptr, "JoltJob", High)`. Trampoline calls `Job::Execute()` then walks barrier slots and calls `OnJobFinished`. |
| `Barrier::WaitForJobs` | `Luth::JobSystem::WaitForCounter(&barrier->m_Counter, 0)` — tries depth-limited inline steals first, then yields via `SwitchTo(SchedulerFiber)`. The OS thread is freed; other ready fibers run. |
| `Barrier::AddJob` | Increments `LuthBarrier::m_Counter` (an `AtomicCounter`) before `SetBarrier(this)` — increment-after-set was a v2.10.0 race fix. |
| Job system shutdown | `WaitForJobs` watchdog logs a warning after N polls without progress (catches stuck barriers). |

`m_JobAdapter` constructed with `kAdapterMaxJobs = 2048`, `kAdapterMaxBarriers = 8`.

---

## Editor integration

### Authoring

`Inspector` drawers in `luthien/source/luthien/inspectors/component_drawers/`: `ColliderDrawer.cpp`, `RigidBodyDrawer.cpp`, `CharacterControllerDrawer.cpp`. All edits route through `CommandHistory` + `ComponentPropertyCommand<T, F>`; `Poke(entity)` calls `registry.patch<T>` to fire `on_update` and queue a rebuild.

CharacterController drawer's `OnAdd` issues a `CompoundCommand`: if the entity lacks a Collider, add a default `Collider{ Capsule, r=0.4, hh=0.9 }` **first**, then the CharacterController (order matters — EnTT fires `on_construct` synchronously; CC-first would log a non-capsule warning before the Collider arrives).

### Viewport state

`Luth::EditorHooks` (`luth/source/luth/core/EditorHooks.h`) carries 10 physics-debug fields on `EditorViewportState` (color mode, shape/AABB/CoM toggles per Selected/All, alpha-unselected, segment count). Implemented in `Luthien` editor; engine reads via `EditorHooks::Get()->GetViewportState(out)`. Runtime build (no editor hook) gets defaults and skips debug-draw entirely.

### Play-mode gating

`PhysicsSystem::Update` early-returns after drains + debug-draw when `PlayState == Editing`. Bodies still exist + can be debug-drawn; they just don't step. `m_Accumulator` reset to 0 so the next Play doesn't catch up on accumulated wall time.

---

## `PlayerControllerSystem` (stub)

`luth/source/luth/scene/systems/PlayerControllerSystem.{h,cpp}` — placeholder until scripting lands. Walks all `(CharacterController, WorldTransform)`, polls raw GLFW keycodes (W=87/A=65/S=83/D=68/Space=32) via `Input::IsKeyPressed`, extracts forward/right from `world.Matrix` columns (`-Vec3(world.Matrix[2])` / `Vec3(world.Matrix[0])`), projects to ground plane + normalizes, multiplies by `cc.moveSpeed`, calls `cc.SetDesiredVelocity()`. Space + `IsGrounded()` → `cc.Jump()`.

Registered in `SystemRegistry` between `TransformSystem` and `PhysicsSystem`. Gated by `m_RunGameSystems` in `App::GameStageFn` so Editing mode stays inert. Deletes when scripts can call `SetDesiredVelocity` directly.

---

## Cornerstone alignment

| Cornerstone | Compliance |
|---|---|
| **No `std::mutex` on hot paths** | `LuthContactListener` trigger cache + `m_DirtyAssetsScratch` use `Luth::SpinLock` (V1, sub-µs hold time). |
| **No `thread_local`** | None introduced. JobContext provides FLS where needed. |
| **No `new`/`delete` in gameplay/render** | `new JPH::CharacterVirtual(...)` routes through Jolt's `JPH_OVERRIDE_NEW_DELETE` allocator hook (documented exception). All other JPH objects are Jolt-internal allocations. |
| **No legacy Vulkan** | Debug-draw composes with the rendering subsystem's `DebugDrawSubsystem` (Dynamic Rendering pass). |
| **Editor decoupling** | `Luth.lib` has zero `luthien/...` includes. Editor reaches physics state through `Luth::EditorHooks`. |
| **Composition over duplication** | Character path mirrors body path verbatim — `m_CharacterMap` ↔ `m_BodyMap`, `TryCreateCharacter` ↔ `TryCreateBody`, `ApplyCharacterTuning` ↔ `ApplyRigidBodyTuning`. No new lifecycle mechanism introduced. |

---

## See also

- `arch/scene-ecs.md` — component/system update order
- `arch/fiber-system.md` — V1-V6 hazards, `WaitForCounter`, `AtomicCounter`
- `arch/memory.md` — `TaggedPageAllocator` (V6 wiring), debug-draw GPU lifetime
- `arch/frame-pipeline.md` — Game/Render stage isolation, GameStageFn dispatch
- `arch/editor.md` — `IEditorHooks::GetViewportState`, `PlayState`

**History files** (per-effort archaeology):
- `history/v2.x/rigid-bodies.md` (v2.10.0) — Tier 0 foundation, `WaitForCounter` UAF
- `history/v2.x/jolt-physics-queries.md` (v2.10.1) — Godot-pattern listener
- `history/v2.x/jolt-physics-assets.md` (v2.10.2) — ShapeCache + PhysicsMaterial
- `history/v2.x/jolt-character-controller.md` (v2.10.3) — Tier 1 CharacterVirtual
