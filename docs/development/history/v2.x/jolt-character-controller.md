# v2.10.3 — jolt-character-controller

**Date:** 2026-05-18
**Commits:** 6 (on `feat/jolt-character-controller`)
**Series:** `jolt-physics`, fourth effort. Mode-B per-effort PATCH bump.

---

## Overview

Tier 1 of the Jolt integration: a kinematic, query-driven character controller built on `JPH::CharacterVirtual`. Introduces `Component::CharacterController` (paired with a `Collider Type::Capsule`), wires the lifecycle through the existing `BuildResult` + `DrainPendingBuilds` shadow-swap, integrates a per-substep sweep inside the `m_StepInFlight` guard, and surfaces minimal authoring (Inspector, scene serialization, debug-draw colored by ground state). A stub `PlayerControllerSystem` polls raw WASD/Space and feeds `desiredVelocity` to every character — the placeholder until a scripting layer lands.

The architectural fork that shaped the design: should the character be a brand-new component that owns its capsule shape inline (Unity's `CharacterController` model — IS-A `Collider`), or a behaviour component that pairs with the existing `Collider` (Unreal's `ACharacter` + `UCapsuleComponent` split)? Reference-engine survey (Unity, Unreal, Godot) showed two of three engines split shape from movement-logic. Path B (Unreal-style: require a `Collider Type::Capsule`) is the consistent choice for Luth — `Collider + RigidBody` is already the shape-vs-behaviour split for bodies, and a `Collider + CharacterController` pair extends that pattern verbatim. The cost (a one-shot `LH_CORE_WARN` when the user attaches a non-capsule collider) is small; the alternative would have introduced a parallel shape-authoring surface on `CharacterController` that duplicated `Collider`'s Inspector + serializer + ECS signals.

The gameplay-input boundary was the other open question — `JPH::CharacterVirtual` needs a desired-velocity vector each frame from *somewhere*, and Luth has no scripting layer. All three reference engines (Godot's GDScript `_physics_process`, Unity's `CharacterController.Move()` from a `MonoBehaviour`, Unreal's `AddMovementInput` from a `Pawn`'s blueprint or C++) source the velocity from a script attached to the entity. Luth's stub `PlayerControllerSystem` is honest about being placeholder: it walks every `CharacterController` and feeds WASD to all of them, gated by `m_RunGameSystems` so Editing mode stays inert. When scripting lands the stub gets deleted and scripts call `CharacterController::SetDesiredVelocity(Vec3)` directly — no API churn on `CharacterController` itself.

`Collider`'s `Type::Capsule` tagged-union arm and the existing `ShapeCache.GetOrBuild` primitive path already produce a `JPH::CapsuleShape` with `RotatedTranslatedShape` wrapping for non-identity offset/rotation — the character path reuses both, so a capsule edit (radius/halfHeight in the Inspector) flows through `on_update<Collider>` → `QueueBuild` → `DrainPendingBuilds`'s dispatch (CC arm) and either fingerprint-fast-paths or tears down + rebuilds the JPH character without character-side code duplicating any of that logic.

---

## Sub-Tasks

| # | What landed | Commit |
|---|---|---|
| A | **Component definition.** `Component::CharacterController` (authoring fields: maxSlopeAngleDeg, mass=70, maxStrength=100, characterPadding=0.02, predictiveContactDistance=0.1, penetrationRecoverySpeed=1.0, layer=1, gravityFactor=1.0, moveSpeed=5.0, jumpSpeed=6.0; per-frame inputs: desiredVelocity, jumpQueued; read-back: groundState, currentVelocity; inline `SetDesiredVelocity` / `Jump` / `IsGrounded` API). `Component::CharacterControllerRuntime` (non-owning `JPH::CharacterVirtual*` observer + u64 fingerprint). `Component::GroundState` enum mirroring `JPH::CharacterBase::EGroundState`. Forward-declare `JPH::CharacterVirtual` so the header stays JPH-include-free. | [`43cc08c`](../../../../commit/43cc08c) |
| B | **PhysicsSystem character lifecycle + step integration.** `m_CharacterMap: unordered_map<entt::entity, JPH::CharacterVirtual*>` owns the heap allocation; `PendingCharacterDestroy` queue mirrors `PendingDestroy`. `EnsureSignalsConnected` extended with `on_construct/update/destroy<CharacterController>`. `OnComponentDestroyed` now calls *both* `DestroyBodyForEntity` and `DestroyCharacterForEntity` — each early-returns on `m_*Map.find` miss, so no discriminator is needed. `TryCreateCharacter` validates `Collider::type == Capsule` (warn-once + Failed otherwise), reuses `m_ShapeCache.GetOrBuild` for the capsule shape, populates `CharacterVirtualSettings`, `new JPH::CharacterVirtual(...)` via Jolt's `JPH_OVERRIDE_NEW_DELETE` allocator. `DrainPendingBuilds` extended with explicit RB-vs-CC dispatch + warn-once on entities that have both. `ApplyCharacterTuning` fast-path covers fields with JPH setters (mass, maxStrength, maxSlope, penetrationRecoverySpeed); structural fields (capsule shape, padding, predictive distance) live in the fingerprint and force tear-down. `UpdateCharacters` runs inside the substep loop: composes velocity (horizontal from `desiredVelocity`, vertical = `currentVelocity.y + gravity*factor*dt`, jump impulse if grounded), calls `ExtendedUpdate` with default `ExtendedUpdateSettings` (Tier 1's "JPH defaults" stair/slope handling) and empty filters, writes `groundState` + `currentVelocity` back. `SyncCharactersToTransforms` writes `GetPosition()` → `transform.Position` + `world.Matrix`. `m_StepInFlight` guard extracted from `Step()` to the substep loop body so it covers both `UpdateCharacters` and `m_System.Update`. `DrawDebugBodies` second pass walks character entities and emits a wire capsule colored by ground state (green/yellow/red/grey). Destructor + scene-change teardown both iterate-delete `m_CharacterMap`. | [`83b048c`](../../../../commit/83b048c) |
| C | **Inspector + scene round-trip.** `CharacterControllerDrawer` mirrors `RigidBodyDrawer`: nine `UI::Property` fields + read-only velocity/groundState rows (`ImGui::BeginDisabled` wrapper). Custom `OnAdd`: if entity has no Collider, `CompoundCommand` adds `Collider{Type::Capsule, radius=0.4, halfHeight=0.9}` *first*, then `CharacterController` — order matters because EnTT fires `on_construct` synchronously and the build dispatch needs to find a Capsule on the entity when CharacterController's signal arrives. `OnCopy`/`OnPaste` JSON for authoring fields only (per-frame inputs + runtime read-back excluded). `SceneSerializer` save/load blocks mirror the RigidBody pattern (populate-then-AddComponent so on_construct sees deserialized values). | [`bce0c5d`](../../../../commit/bce0c5d) |
| D | **PlayerControllerSystem stub.** New `ISystem` walks `(CharacterController, WorldTransform)`. Forward/right basis extracted from `world.Matrix` columns (`-Vec3(world.Matrix[2])` and `+Vec3(world.Matrix[0])`); no `Math::Forward` helper exists in `LuthMath.h` and adding one would have committed to a convention before the engine needs it elsewhere. Horizontal projection (zero Y) + normalize so WASD speed is uniform regardless of pitch/scale. Raw GLFW keycodes (W=87, A=65, S=83, D=68, Space=32) — Luth has no `Key::W` enum yet; existing Input call sites use the same literals. `App::GameStageFn` order: `TransformSystem → (if m_RunGameSystems) PlayerControllerSystem → PhysicsSystem → (if m_RunGameSystems) AnimationSystem`. The system is registered between Transform and Physics in `SystemRegistry::Init`; the dispatch in `GameStageFn` is what enforces the play-mode gate. Marked `// stub: drives CharacterController until scripting lands; delete when scripts can call SetDesiredVelocity directly`. | [`00ec350`](../../../../commit/00ec350) |
| E | **Test scene.** `samples/assets/scenes/character_test.luth` + `.meta`. Static floor (100×1×100 box, clone of `drop_test.luth`'s ground), DirectionalLight, Camera at `(0, 5, 12)` rotated `-20°` X, Player entity (Collider Capsule r=0.4 hh=0.9 + CharacterController defaults) at `(0, 3, 0)`. Spawns ~0.7 above floor surface so gravity has something to do on first frame. | [`4cc5170`](../../../../commit/4cc5170) |
| F | **Wrap-up.** `Version.h` patch bump to v2.10.3. This history file. CLAUDE.md Current Progress + Next. | this commit |

---

## Architectural decisions

### Path B (Collider-paired) over Path A (built-in capsule fields)

Surveyed: Unity (`CharacterController` IS-A `Collider`, inline radius/height fields), Unreal (`ACharacter`'s root MUST be `UCapsuleComponent` — capsule is a separate primitive component, `UCharacterMovementComponent` is the movement logic), Godot (`CharacterBody3D` requires a `CollisionShape3D` child node). Two of three split shape from movement-logic; the one that doesn't (Unity) is also the one that has no other shape-component concept on its GameObjects.

Luth already separates `Collider` (shape, tagged union) from `RigidBody` (motion behaviour). A `Collider + CharacterController` pair is the same pattern verbatim — same on_construct/on_update/on_destroy signals, same `ShapeCache.GetOrBuild` primitive path, same Inspector widget for capsule radius/halfHeight. Adding `CharacterController.{radius, height}` would have introduced a parallel authoring surface that duplicated all of that. The cost of Path B is a one-shot `LH_CORE_WARN` when the user attaches a non-Capsule Collider to a CharacterController entity, plus the editor convenience of auto-adding a default Capsule Collider when the user adds CharacterController to an entity that lacks one (`CompoundCommand` with Collider-first ordering).

### No `materialUUID` field on `CharacterController`

`BACKLOG.md` line 175 notes the planned future direction for physics materials: *per-shape* on `Collider` (currently per-body via `RigidBody.materialUUID`). Path B requires a `Collider` on the character entity, so when shape-level material arrives the character will get it via its Collider — not via a per-character field. Adding `CharacterController.materialUUID` now would fossilize: when the migration happens we'd either delete it (breaking serialized scenes) or keep it as a duplicate override slot (architectural confusion). The future `feat/jolt-physics-material-per-shape` effort owns the question of whether characters need a per-character override on top of their Collider's material; Tier 1 stays out of it.

There is no scheduled "Tier 2 character controller" effort — `CharacterContactListener` / dynamic-body push / crouch / swim were called out as deferred in the issue prompt but don't appear in `BACKLOG.md`. They land piecemeal in whatever follow-up effort needs them. The "drop the field now and add it later" decision is honest about that: it's an alignment with the *next* scheduled material work, not a promise about an unscheduled character-tier-2.

### `PlayerControllerSystem` walks all `CharacterController` entities

Tier 1 typically has one character. A `playerControlled: bool` field on `CharacterController` would be forward-looking (NPCs coexist with the player) but adds a serialized field that the stub system is the only consumer of — and the stub is explicitly placeholder-until-scripting. A separate `PlayerInput` marker component would be cleaner separation (CharacterController stays passive, PlayerInput is the input-binding contract) but multiplies Tier 1's component count for a system that's going away. Walks-all is the smallest surface for the placeholder; multi-character filtering lands when project data demands it.

### `ExtendedUpdate` over plain `Update` for "JPH defaults" stair/slope handling

`JPH::CharacterVirtual::Update` does pure velocity-integration sweep. `ExtendedUpdate` (CharacterVirtual.h:387) wraps that with default `ExtendedUpdateSettings` — stick-to-floor (down step 0.5m) and walk-stairs (up step 0.4m, forward 0.02m, forward-test 0.15m). The Tier 1 spec said "Ground/slope/step handling uses JPH defaults" — those *are* `ExtendedUpdate` defaults; plain `Update` would have given a character that can't navigate the simplest staircase. Empty filters (`JPH::BroadPhaseLayerFilter{}` / `JPH::ObjectLayerFilter{}` / `JPH::BodyFilter{}` / `JPH::ShapeFilter{}`) mean the character collides per the existing `BPLayerInterface` rules (MOVING vs STATIC/TRIGGER) with no extra filtering at Tier 1.

### Per-substep update cadence (deterministic) over once-per-frame (simple)

PhysicsSystem uses a fixed-dt accumulator (`kFixedDt = 1/60`, up to 4 substeps). `UpdateCharacters` runs inside that loop so character sweeps share the substep cadence with the rest of the simulation (deterministic, matches Unity's `FixedUpdate` convention). The desired velocity is sampled once per frame (PlayerControllerSystem runs once per frame) and held constant across substeps; gravity integration happens fresh each substep on the cached `currentVelocity.y`. The alternative — once-per-render-frame variable-dt update — would have jittered at non-60Hz framerates and made the multi-substep recovery path (after a stall) behave oddly. Per-substep is the established convention; Tier 1 follows it.

### `m_StepInFlight` guard extracted to the substep loop body

`Step()` previously owned the guard internally — wrapped `m_System.Update` in `m_StepInFlight.store(true/false, release)`. With `UpdateCharacters` added before `Step()` in the loop, the guard had to extend to cover both: `CharacterVirtual::ExtendedUpdate` runs collision sweeps internally that take body locks, and a nested `Raycast` from any callback reachable from there would deadlock without the guard. Extracting the guard to the loop body covers both without nested set/clear (which would have allowed a brief window between UpdateCharacters and Step where the guard was false). `Step()` becomes a thin `m_System.Update(...)` wrapper now that the caller owns the guard.

### Unified `OnComponentDestroyed` dispatch via no-op falls

`OnComponentDestroyed` calls *both* `DestroyBodyForEntity` and `DestroyCharacterForEntity`. Each does an `m_*Map.find` and early-returns when the entity isn't in its map. RigidBody and CharacterController are mutually exclusive (the build dispatch warns + drops if both are present), so at most one map holds the entity — the other call is a free no-op. The alternative would have been a discriminator on which component is being destroyed (e.g., separate `OnRigidBodyDestroyed` / `OnCharacterControllerDestroyed` callbacks), which scales linearly with the number of physics-bearing components. The no-op approach is O(2 hash-lookups) per destroy — bounded and cheap.

### CompoundCommand "Add CharacterController" adds Collider first

When the user adds a `CharacterController` to an entity without a `Collider`, the drawer's `OnAdd` issues a `CompoundCommand` containing two `ComponentAddCommand`s: Collider then CharacterController. The order matters because EnTT fires `on_construct` synchronously inside `AddComponent`. If `CharacterController` were added first, its on_construct → `QueueBuild` would land in `m_PendingBuild` *before* the Collider arrives. On the first `DrainPendingBuilds` after that, the entity dispatches to the CC arm, fails the `Collider` validation in `TryCreateCharacter`, and logs a one-shot non-capsule warning before the Collider's on_construct fires later in the same frame and triggers a successful rebuild on the next drain. With Collider-first, the queue dedups both signals into one drain that finds a valid pair and builds cleanly the first time — no spurious warning, no two-frame heal.

---

## Files touched

**New (engine)**
- `luth/source/luth/scene/systems/PlayerControllerSystem.h`
- `luth/source/luth/scene/systems/PlayerControllerSystem.cpp`

**New (editor)**
- `luthien/source/luthien/inspectors/component_drawers/CharacterControllerDrawer.cpp`

**New (samples)**
- `samples/assets/scenes/character_test.luth`
- `samples/assets/scenes/character_test.luth.meta`

**Modified (engine)**
- `luth/source/luth/scene/components/Physics.h` — `CharacterController` + `CharacterControllerRuntime` + `GroundState`
- `luth/source/luth/scene/systems/PhysicsSystem.h` — character members + methods
- `luth/source/luth/scene/systems/PhysicsSystem.cpp` — lifecycle, dispatch, step integration, debug-draw
- `luth/source/luth/scene/systems/SystemRegistry.cpp` — register PlayerControllerSystem
- `luth/source/luth/scene/SceneSerializer.cpp` — save/load CharacterController block
- `luth/source/luth/core/App.cpp` — gated PlayerControllerSystem call in GameStageFn
- `luth/source/luth/core/Version.h` — patch bump to v2.10.3

**Modified (editor)**
- `luthien/source/luthien/inspectors/component_drawers/RegisterComponentDrawers.h`
- `luthien/source/luthien/inspectors/component_drawers/RegisterComponentDrawers.cpp`

---

## Verification

Build clean on Debug x64. `Luth.lib`, `Luthien.lib`, `Luthien.exe` all link. No new Vulkan validation errors expected (no renderer changes).

Smoke test in `character_test.luth`:
1. Editor mode: green wire capsule visible at `(0, 3, 0)`; no Jolt asserts on load.
2. Press Play: capsule falls ~0.7 to floor, debug colour cycles red (in-air) → green (grounded).
3. WASD translates capsule at ~5 u/s along the entity's local forward/right basis (default rotation = world axes).
4. Space + grounded → vertical kick; mid-air Space is consumed without effect (intentional — no jump buffering at Tier 1).
5. Add CharacterController via "Add Component" menu on a fresh entity → default Capsule Collider auto-added in the same undoable compound (`CompoundCommand`).
6. Attach a Box-typed Collider to a CharacterController entity → one `LH_CORE_WARN` printed; no crash; entity stays in queue without spawning a JPH character.
7. Save scene → restart editor → reload → authoring fields restored; `groundState` + `currentVelocity` start fresh.
