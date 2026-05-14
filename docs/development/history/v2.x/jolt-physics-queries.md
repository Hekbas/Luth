# v2.10.1 — jolt-physics-queries

**Date:** 2026-05-15
**Commits:** 5 (on `feat/jolt-physics-queries`)
**Series:** `jolt-physics`, second effort. **Mode-A → Mode-B override** — per user direction the series switches from coalesced (one MINOR for the whole series, intermediate `jolt.N-<slug>` checkpoint tags) to per-effort PATCH bumps. `v2.10.1` is the first PATCH bump under the new rule. Future series efforts continue as Mode B.

---

## Overview

Phase D of the Jolt integration: collision queries (`Raycast` + `OverlapBox/Sphere/Capsule` over `JPH::NarrowPhaseQuery`) and the contact/trigger event surface (`LuthContactListener` feeding a per-frame `DrainEvents` API). The four gameplay-visible event kinds are `ContactAdded`, `ContactRemoved`, `TriggerEnter`, `TriggerExit`.

The single architectural decision shaping the design: how to deliver `TriggerEnter`/`TriggerExit` given that Jolt's `OnContactRemoved` forbids body access ([ContactListener.h:127-130](../../../luth/extern/source/jolt/Jolt/Physics/Collision/ContactListener.h)) and can't read `IsSensor()` from the removed bodies. Researched against Godot's production Jolt integration (`modules/jolt_physics/spaces/jolt_contact_listener_3d.cpp`), PhysX trigger dispatch, Chaos overlap diff, and Box2D v3 sensor events — settled on Godot's pattern: a listener-owned cross-frame trigger-pair cache (`std::unordered_set<u64>` of packed BodyID pairs) under `SpinLock`, populated from both `OnContactAdded` AND `OnContactPersisted` (so runtime `Body::SetIsSensor` flips emit the matching Enter/Exit), looked up + cleared in `OnContactRemoved`.

Cache mutation lives inside the listener under SpinLock; the event queue (`MPMCQueue<PhysicsEvent, 4096>`) is dispatch only. If `TryPush` overflows the queue, the cache stays consistent and the next pair Add self-heals — we just lose the gameplay-side notification for the overflowed event. `m_OverflowCount` is bumped atomically and logged once per frame in `DrainEvents`.

Tier 0 ships with no gameplay-side drain consumer. Until a script layer lands, the queue saturates at 4096 events and the once-per-frame warning fires — documented limitation, not a tuning issue. The smoke harness used to verify Phase D was stripped at wrap-up; the engine does not own the drain.

---

## Sub-Tasks (grouped by phase)

| # | What landed | Commits |
|---|---|---|
| 1 | **Raycast + step-in-flight guard.** `PhysicsQuery.h` (RaycastHit + OverlapHit POD), `LayerMaskFilter.h` (u32 → JPH BP+Object filter adapter), `PhysicsSystem::Raycast` over `GetNarrowPhaseQuery().CastRay`. World normal computed post-hit via `BodyLockRead` + `Body::GetWorldSpaceSurfaceNormal(subShapeID2, hitPoint)` — `RayCastResult` lacks normal data. `m_StepInFlight` atomic set/cleared around `m_System.Update` so queries assert against in-step re-entry. | [`e808f02`](../../../../commit/e808f02) |
| 2 | **OverlapBox / OverlapSphere / OverlapCapsule.** Shared `OverlapShape` core dispatches to `GetNarrowPhaseQuery().CollideShape` with a stack-allocated `SpanOverlapCollector` subclass of `CollideShapeCollector` that writes into the caller-provided `std::span<OverlapHit>` and calls `ForceEarlyOut` when full. Shape constructed on stack per call via `BoxShapeSettings`/`SphereShapeSettings`/`CapsuleShapeSettings::Create()`; baseOffset == center for float precision. | [`fae30ee`](../../../../commit/fae30ee) |
| 3 | **ContactListener + cache + events + drain.** `PhysicsEvents.h` (4-kind enum + 40-byte POD struct), `PhysicsListeners.{h,cpp}` (Godot-pattern listener, trigger cache under SpinLock, emit-outside-lock TryPush). `m_EntityByBodyIndex` (`std::vector<entt::entity>(kMaxBodies)`) populated at `TryCreateBody` alongside `SetUserData`, cleared in `DrainPendingDestroys` — supports OnContactRemoved's no-body-access constraint. Listener install via `m_System.SetContactListener(&m_ContactListener)` after `m_System.Init`; teardown via `SetContactListener(nullptr)` at top of dtor. `DrainEvents` lock-free TryPop into caller span + once-per-frame overflow warning. | [`ebc2b37`](../../../../commit/ebc2b37) |
| 4 | **trigger_test scene + smoke harness.** User-authored `samples/assets/scenes/trigger_test.luth` (floor + sensor cube + dynamic sphere mover). Temporary in-Update drain + per-event `LH_CORE_INFO` for smoke verification — stripped at wrap-up. Verified manually against `drop_test.luth` (ContactAdded fires on landing; ContactRemoved fires when bodies settle and sleep per Jolt's documented behavior) and `trigger_test.luth` (exactly 1× TriggerEnter then 1× TriggerExit with matching entity pairs). | [`a04ef0e`](../../../../commit/a04ef0e) |
| W | **Wrap-up.** Strip smoke harness from `PhysicsSystem::Update`. `Version.h` patch bump. History file. CLAUDE.md Current Progress update. | this commit |

---

## Architectural decisions

### Godot-pattern listener (Option D refined)

Three competing designs were considered:

- **Option A** — emit a generic `ContactRemoved` only; gameplay disambiguates via `entity.GetComponent<RigidBody>().isSensor`. Simple but loses sensor flag if the entity is destroyed mid-step, and doesn't match AAA-engine semantics (Unity/Unreal/Godot all expose 4 distinct event kinds).
- **Option B** — fully resolve at callback time via a lock-free fixed-capacity probing hash table with atomic CAS. ~80 LOC for the new primitive; rigorous but the primitive sits there waiting for a second consumer.
- **Option D** — drain-time disambiguation. Initially proposed by us; pushed back by the user with "research deeply how other engines do it." After reading [godot/modules/jolt_physics/spaces/jolt_contact_listener_3d.cpp](https://github.com/godotengine/godot/blob/master/modules/jolt_physics/spaces/jolt_contact_listener_3d.cpp), the cleaner variant emerged: cache lives inside the listener (not at drain time), mutated under SpinLock, queue is dispatch only. Queue overflow can no longer poison cache state.

We shipped Option D-Godot. Lock-free trigger cache primitive deferred unless and until a second consumer appears.

### Cache populated from both OnContactAdded AND OnContactPersisted

`OnContactPersisted` fires every step for every active contact pair. Skipping it (a tempting per-frame cost optimization) breaks runtime sensor-flag flips: `Body::SetIsSensor` is a lock-free atomic flip but without Persisted-side cache evaluation, the listener wouldn't notice the change until the next physical add/remove transition. Persisted runs the same `EvaluateTrigger(isTrigger=current)` path as Added; the cache's insert-if-new vs. erase-if-present logic produces the right Enter/Exit transitions even for runtime flips.

Persisted is NOT exposed as a gameplay event — too noisy for a stack (100s of fires per step). The listener processes Persisted internally for cache correctness only.

### Canonical pair key (BodyID-based, not entity-based)

`u64 = ((max(GetIndexAndSequenceNumber(a), b) << 32) | min(...))`. Sorted ascending so the key is order-independent without storing both orderings. `GetIndexAndSequenceNumber` includes Jolt's 8-bit sequence number ([BodyID.h:17-21](../../../luth/extern/source/jolt/Jolt/Physics/Body/BodyID.h)) which prevents slot-reuse collisions — pathologically aggressive churn (>256 body cycles at the same slot index between two contact events on the same pair) could collide; realistic risk is zero for Tier 0 scenes.

Entity-keyed cache was rejected because an entity destroyed mid-step has its `entt::entity` invalidated before `OnContactRemoved` fires; the cache lookup would then fail spuriously.

### Body-index side table for OnContactRemoved

`OnContactAdded` and `OnContactPersisted` can read `Body::GetUserData()` directly (lock-free atomic load on `Body.mFlags`). `OnContactRemoved` explicitly forbids body access ([ContactListener.h:127-130](../../../luth/extern/source/jolt/Jolt/Physics/Collision/ContactListener.h)). Resolution: `std::vector<entt::entity> m_EntityByBodyIndex` sized `kMaxBodies` (~64 KB), indexed by `BodyID.GetIndex()`, populated at `TryCreateBody` alongside `SetUserData`, cleared at body destroy. Listener carries a `std::span<entt::entity>` view into it; reads are lock-free since the table only mutates outside Step (deferred-destroy queue runs at Update start, before any Step).

### Emit outside the SpinLock

`EvaluateTrigger` holds the SpinLock for the cache `insert`/`erase` only; the `TryPush` to the event queue happens after the lock releases. V1-compliant (critical section bounded to a hash op, ~50-100 cycles), and avoids inflating the lock with a wait-free queue op that would never block anyway.

---

## Bugs found along the way

| Symptom | Root cause | Fix |
|---|---|---|
| Compile errors in `entt.hpp:13020` instantiating `entt_traits<std::allocator<entt::entity>>` after introducing the listener | `std::vector<entt::entity>(kMaxBodies, entt::null)` — `entt::null`'s templated `operator T() const` matches both `T = entt::entity` (value arg) AND `T = std::allocator<entt::entity>` (allocator arg) in vector's overload set; MSVC picked the wrong one | Materialise via `entt::entity{entt::null}` at the call site to pin the overload (committed in `ebc2b37`) |

---

## File list

**New (engine)**
- `luth/source/luth/physics/PhysicsQuery.h` — `RaycastHit` + `OverlapHit` POD
- `luth/source/luth/physics/LayerMaskFilter.h` — `u32` mask → `JPH::BroadPhaseLayerFilter` + `JPH::ObjectLayerFilter`
- `luth/source/luth/physics/PhysicsEvents.h` — `PhysicsEventType` enum + `PhysicsEvent` POD
- `luth/source/luth/physics/PhysicsListeners.{h,cpp}` — `LuthContactListener` with trigger cache + emit-outside-lock pattern

**New (samples)**
- `samples/assets/scenes/trigger_test.luth` (+ `.meta`) — Tier-0 smoke #4 scene

**New (docs)**
- `docs/development/history/v2.x/jolt-physics-queries.md` (this file)

**Modified**
- `luth/source/luth/scene/systems/PhysicsSystem.h` — query API + `DrainEvents` + listener / queue / body-index members + `m_StepInFlight`
- `luth/source/luth/scene/systems/PhysicsSystem.cpp` — listener install/teardown, body-index populate/clear at create/destroy, query implementations, `Step` wrapped with re-entry guard
- `luth/source/luth/core/Version.h` — patch bump to `v2.10.1`
- `CLAUDE.md` — Current Progress update (Latest shipped → `jolt-physics-queries`)

---

## Verification

Two smoke scenes, manual editor pass on Debug x64.

**`drop_test.luth`** — regression for solid-contact dispatch. A dynamic box drops onto the static floor; observed `[Physics] ContactAdded` on landing followed by `[Physics] ContactRemoved` when the body settles and sleeps (Jolt fires Removed for all contacts of a sleeping body per [ContactListener.h:104-105](../../../luth/extern/source/jolt/Jolt/Physics/Collision/ContactListener.h)). No `Trigger*` events on the all-solid scene.

**`trigger_test.luth`** — Tier-0 smoke #4. Dynamic sphere mover crosses a static sensor cube. Observed exactly:

```
[Physics] TriggerEnter entities=(9437188, 12582914)
[Physics] TriggerExit  entities=(9437188, 12582914)
```

Matching entity pairs in both events, no `ContactRemoved` spurious, no duplicate `TriggerEnter`s while inside the sensor.

Smoke harness removed at wrap-up; engine no longer drains internally. Until a gameplay layer takes ownership of the drain, scenes generating events will saturate the 4096-capacity queue and emit a once-per-frame `[PhysicsSystem: dropped N contact event(s) - queue saturated]` warning. Documented limitation, not a Phase-D bug.

---

## Out of scope

- `OnContactPersisted` exposure as a gameplay event. Persisted is handled internally for trigger cache correctness only; never reaches `DrainEvents`. Adding it is one switch-case + a new enum entry if a consumer ever asks.
- `LuthBodyActivationListener` (body activation/sleep events). Tier-0 spec does not require it; deferred.
- Same-step Exit-then-Enter ordering for compound shapes. Godot drains exits before enters via two HashSets; our single MPMCQueue gives FIFO order. Tier 0 has no compound shapes (deferred to Tier 2), so no observable difference.
- Lock-free fixed-capacity pair-cache primitive (Option B's would-be addition). Cache stays an `std::unordered_set<u64>` under SpinLock — composes with existing primitives; no new lock-free hash needed.
- Gameplay-side `DrainEvents` consumer. No script layer yet; events will overflow the queue until one lands. Not a bug.

---

## Foundation-testing implications

The listener glue is a new instance of the "stack-local handler captures refs to long-lived storage" pattern (queue, body-index span). If the queued-tasks `foundation-testing` series eventually lands, this is worth a stress-test target: hammer the listener with concurrent Adds across many worker fibers, verify the cache stays consistent under SpinLock contention, verify TryPush overflow handling doesn't desync state. Easier than the v2.10.0 `WaitForCounter` UAF — the lifetime story is shorter (listener outlives all callbacks by construction) and ASan should catch any leakage.
