# v2.10.0 — jolt-rigid-bodies

**Date:** 2026-05-14
**Commits:** 23 (on `feat/jolt-rigid-bodies`, renamed from `feat/jolt-physics-tier0`)
**Series:** `jolt-physics`, **start** (Mode A — series-coalesced; first effort. Future intermediate efforts tag as `jolt.N-<slug>` without further `Version.h` bumps. Series-end Release lands at series closeout.)

---

## Overview

Tier 0 of the Jolt integration: rigid-body dynamics, collision queries' foundation (Raycast + Overlap deferred to Tier 0 follow-up), trigger sensors, transform sync, and an editor-side authoring surface (Inspector drawers + per-pass debug visualization). The engine now has working physics for primitive shapes (Box / Sphere / Capsule), serialized round-trip via `SceneSerializer`, and Inspector authoring with undo.

Two non-obvious motivations shaped the design and survive into Tier 1+:

1. **Fiber stress-test for Luth's JobSystem.** Jolt's parallel work runs through Luth's Naughty Dog-style work-stealing fibers via a direct `JPH::JobSystem` adapter (`LuthJobSystemForJolt`, not `JobSystemWithBarrier`). Barrier waits route through `Luth::JobSystem::WaitForCounter` — the V5 depth-limited inline-execution + fiber-yield path. The whole-tier value, not just a means to an end.
2. **Ragdoll-ready ECS shape.** Single `Collider` component (tagged variant, ≤64 bytes), separate `RigidBody` for dynamics state, runtime-only `PhysicsBodyRuntime`. Compound shapes deferred to Tier 2 modeled as child entities each carrying their own `Collider`. Mesh/Convex shapes hold UUID + meshIndex references back to the Model asset.

Mode A series start: `Version.h` bumps once to `v2.10.0` here; intermediate efforts ride this MINOR and tag as `jolt.N-<slug>` checkpoints. Phase D (queries + ContactListener), Phase E remainder (`MeshShape` / `ConvexHullShape` baking, `PhysicsMaterial` asset), and Phase F (1000-body Tracy validation) are the planned follow-up efforts.

---

## Sub-Tasks (grouped by phase)

| # | Phase | What landed | Commits |
|---|---|---|---|
| A | Vendor + adapter | Jolt v5.5.0 submodule + premake; `JoltMath.h` converters; `LuthJobSystemForJolt` adapter (direct `JPH::JobSystem`); `AtomicCounter::Increment/Decrement` public API; `samples/physics_smoke/jobsys_proof.cpp` 1000-body harness (Phase-A Tracy gate passed) | [`a649794`](../../../../commit/a649794), [`bed2ed1`](../../../../commit/bed2ed1), [`0b758cf`](../../../../commit/0b758cf), [`a6d246c`](../../../../commit/a6d246c), [`e0e4936`](../../../../commit/e0e4936) |
| B | Engine wire-up | `PhysicsSystem` skeleton + Jolt globals init/teardown; system registered in `SystemRegistry`, ticked from `GameStageFn` between `TransformSystem` and `AnimationSystem` | [`ea24803`](../../../../commit/ea24803) |
| C | Body lifecycle + sync | EnTT signal handlers (on_construct / on_destroy), `m_BodyMap`, `MoveKinematic` pre-step + dynamic read-back post-step; `SceneSerializer` for `Collider` + `RigidBody`; `samples/assets/scenes/drop_test.luth` smoke scene; signal-bug fix: connect signals regardless of `PlayState` so first-Play has bodies (no Stop+Play required) | [`55b8f68`](../../../../commit/55b8f68), [`3307f40`](../../../../commit/3307f40), [`e43c2ad`](../../../../commit/e43c2ad) |
| D | Debug-draw producer + subsystem | `luth/core/DebugDraw` shared facility (frame-indexed double-buffer); `PhysicsDebugRenderer` (JPH::DebugRendererSimple subclass) forwards Jolt lines; `DebugDrawSubsystem` LINE_LIST render pass gated to scene view via `RenderView::drawDebugShapes` | [`8672bff`](../../../../commit/8672bff), [`36c5e12`](../../../../commit/36c5e12), [`7d57730`](../../../../commit/7d57730) |
| E | Reliability fixes (along the way) | `GPUTaggedPageAllocator` backings missed `VERTEX_BUFFER_BIT`; `SceneSerializer` populated components post-AddComponent (copy-emplace fix for on_construct race); `LuthBarrier` increment-before-gate race fix; adapter watchdog for stuck `WaitForJobs` | [`582bc68`](../../../../commit/582bc68), [`bd07214`](../../../../commit/bd07214), [`237419c`](../../../../commit/237419c), [`31f8267`](../../../../commit/31f8267) |
| F | CCD + Inspector authoring | `motionQuality` enum on `RigidBody` (Discrete / LinearCast) threaded into `BodyCreationSettings::mMotionQuality`; Inspector drawers for `Collider` + `RigidBody` (with `ComponentSnapshotCommand<T>` for union-member undo); deferred-build queue (on_construct + on_update both queue, drain at Update start) | [`af78ecb`](../../../../commit/af78ecb), [`282d0bc`](../../../../commit/282d0bc), [`cee1f3e`](../../../../commit/cee1f3e) |
| G | Wire-primitive debug draw | GPU memory leak fix in `DebugDrawSubsystem` (missing `cache.CurrentTag`); EditorSettings + `EditorViewportState` carry the new physics-debug fields; Preferences > Physics + ScenePanel gizmo-dropdown 3×2 table; `DrawDebugBodies` replaced — walks our EnTT registry directly, emits 3 great-circle wire spheres / 12-edge boxes / capsule arcs; three passes (Shapes / AABBs / CoM) with Selected/All scopes; color modes (Uniform / ByMotionType / BySleepState); sensor alpha tint; unit-circle cache | [`d30a81b`](../../../../commit/d30a81b), [`c08c05e`](../../../../commit/c08c05e), [`4fea6bf`](../../../../commit/4fea6bf) |
| H | Late root-cause | `WaitForCounter` UAF on stack-local `AtomicCounter` — surfaced as a Release-only `__report_gsfailure` after the rebuild touched stack layouts. See [jobsystem-waitforcounter-uaf.md](jobsystem-waitforcounter-uaf.md) | [`17cb1e3`](../../../../commit/17cb1e3) |
| I | Wrap-up | Version bump, history file, plan-log sync | this commit |

---

## Architectural decisions

### Direct `JPH::JobSystem` (skip `JobSystemWithBarrier`)

`JobSystemWithBarrier`'s built-in `std::semaphore` barrier would block worker fibers on a kernel wait and bypass V5 (depth-limited inline execution + fiber yield) entirely. We implement `JPH::JobSystem` directly with a custom `LuthBarrier` that backs `WaitForJobs` through `Luth::JobSystem::WaitForCounter`. The trampoline (`Luth::JobSystem::Execute(TrampolineFn, job, /*counter*/nullptr, "JoltJob", High)`) runs `Job::Execute()` then walks the job's `cBarrierSlots = 4` and calls `OnJobFinished`, which decrements the barrier's `AtomicCounter`. `WaitForJobs` is `Luth::JobSystem::WaitForCounter(&barrier->m_Counter, 0)` — tries depth-limited inline steals first, then yields via `SwitchTo(SchedulerFiber)`. The OS thread is freed; other ready fibers run.

Phase-A gate (`samples/physics_smoke/jobsys_proof.cpp`) verified Tracy zones spread across ≥ 4 worker threads with no `Sleep`/semaphore zones inside Jolt-wait callsites and ≥ 80% worker utilization during 1000-body stepping.

### Single `Collider` variant, separate `RigidBody`, child-entity compounds

`Collider` is a tagged union: `Type { Box, Sphere, Capsule, ConvexHullRef, MeshRef }`. Parametric shapes inline; mesh/convex reference a Model asset via `meshRef.modelHi/modelLo + meshIndex`. `static_assert(sizeof(Collider) <= 64)` keeps it cache-line-tight. Closer to Bevy / Godot's separate-component model than Unity's "stack multiple colliders on one GameObject" — keeps the ECS layout small and fixed-size, and decouples visual mesh from collision mesh (low-poly collider on high-poly visual is the common case, Unreal makes that harder).

Compound shapes deferred to Tier 1+ as child entities each carrying a `Collider`; `PhysicsSystem` will assemble a `JPH::StaticCompoundShape` at body-build time. No `CompoundShape` variant case.

### Deferred-build queue (on_construct + on_update queue, drain at Update start)

EnTT's `on_construct` fires synchronously inside `AddComponent` before the caller can populate fields. Same for runtime Inspector "Add Component" — the body would be built with defaults and ignore subsequent edits. Fix: signal handlers queue the entity in `m_PendingBuild`; `DrainPendingBuilds` runs at the start of `PhysicsSystem::Update`, destroys any existing body, and re-runs `TryCreateBody`. Sort+unique dedup at drain (one rebuild per frame even with many edits). `on_update<Collider>` and `on_update<RigidBody>` both route to the same queue, so Inspector edits via `registry.patch<T>()` rebuild bodies uniformly — shape, mass, layer, motion quality all picked up.

Side benefit: `SceneSerializer` load no longer needs the copy-emplace workaround for the on_construct race (still uses it for safety / documentation, but the queue handles the race independently).

### Wire-primitive debug draw — bypass Jolt's `DrawBodies`

Jolt's `DebugRenderer::DrawSphere` / `DrawCapsule` / `DrawBox` are non-virtual concrete methods (`DebugRenderer.h:107-120`) that tessellate to ~1k+ triangles and call the virtual `DrawGeometry`. By the time `DrawGeometry` is invoked, shape identity is lost — only a `GeometryRef` pointing to private cached batches remains. No per-primitive override is possible.

We stopped calling `m_System.DrawBodies` for shapes entirely. `PhysicsSystem::DrawDebugBodies` now walks the EnTT view `(Collider, WorldTransform, RigidBody, PhysicsBodyRuntime)`, emits 3 orthogonal great circles per sphere (Unity-style), 12 edges per box, two equator circles + 4 axial seams + 4 hemisphere arcs per capsule. Default 32 segments, tunable in Preferences. ~30× cheaper than the Jolt tessellation path (100 spheres: ~9.6k lines vs ~300k).

`PhysicsDebugRenderer` (the `JPH::DebugRendererSimple` subclass) is kept alive but unused for primitives — Tier 2 will need it back for constraint visualization, and zero cost when not invoked.

### Three-pass debug viz with Selected/All scopes

Synthesized from Unity (uniform-green, gated by Gizmos toggle), Unreal (per-channel colors), and Godot (cyan wireframe, debug toggle). Three independent passes (Shapes / AABBs / CoM), each with paired Selected/All toggles in a 3×2 table in both the Preferences "Physics" section and the ScenePanel gizmo dropdown. Color modes: Uniform (default, Unity-style green), ByMotionType (grey/blue/green by motion), BySleepState (mirrors Jolt's `EShapeColor::SleepColor` — static grey, kinematic green, dynamic-active yellow, sleeping red; queries `BodyInterface::IsActive`). Sensors render at half the body's current alpha — distinct from solids without a separate dashed-line path.

---

## Bugs found along the way

| Symptom | Root cause | Fix commit |
|---|---|---|
| Floor wireframe drifted under motion despite body being static | `GPUTaggedPageAllocator` backings advertised STORAGE/INDIRECT/UNIFORM but not VERTEX_BUFFER_BIT; `vkCmdBindVertexBuffers` got undefined data | [`582bc68`](../../../../commit/582bc68) |
| Scene with `Static` rigid body loaded as `Dynamic` | EnTT `on_construct` fired inside `AddComponent` before caller could populate fields; signal handler read defaults | [`bd07214`](../../../../commit/bd07214) + later [`cee1f3e`](../../../../commit/cee1f3e) |
| Jolt physics step froze indefinitely under load | `LuthBarrier::AddJob` incremented its counter *after* `SetBarrier(this)`, and workers running concurrent jobs called `OnJobFinished` (decrement) before the increment landed → counter clamped at 0, swallowed decrements, permanent above-zero state | [`237419c`](../../../../commit/237419c) |
| GPU memory grew continuously with debug-drawn scene loaded (even in Editing mode) | `DebugDrawSubsystem::AddDebugDrawPass` called `GPUTaggedPageAllocator::Allocate` without setting `cache.CurrentTag`, so VulkanBackend's per-frame `FreeTag(N-2)` never matched the leaked pages | [`d30a81b`](../../../../commit/d30a81b) |
| Release-only `__report_gsfailure` in `PhysicsSystem::Update` after "some time in Play", scaled with body count | `WaitForCounter`'s yield path tail returned before paired `DecrementCounter`'s final `fetch_sub(1)` completed; caller's stack-local `AtomicCounter` died while still being written to. See [jobsystem-waitforcounter-uaf.md](jobsystem-waitforcounter-uaf.md) | [`17cb1e3`](../../../../commit/17cb1e3) |

---

## File list

**New (engine)**
- `luth/extern/source/jolt/` — submodule pinned to v5.5.0
- `luth/extern/premake5-jolt.lua` — static lib build (AVX2, RTTI on, exceptions on, C++20)
- `luth/source/luth/physics/JoltMath.h` — GLM ↔ Jolt converters
- `luth/source/luth/physics/JoltConfig.h` — central macros, body user-data packing
- `luth/source/luth/physics/LuthJobSystemForJolt.{h,cpp}` — Jolt JobSystem adapter
- `luth/source/luth/physics/PhysicsLayers.{h,cpp}` — STATIC / MOVING / TRIGGER
- `luth/source/luth/physics/PhysicsDebugRenderer.{h,cpp}` — JPH::DebugRendererSimple subclass (kept but unused for primitives)
- `luth/source/luth/physics/ShapeBuilder.{h,cpp}` — primitive shape construction
- `luth/source/luth/scene/components/Physics.h` — `Collider`, `RigidBody`, `PhysicsBodyRuntime`
- `luth/source/luth/scene/systems/PhysicsSystem.{h,cpp}` — body lifecycle, sync, debug-draw passes
- `luth/source/luth/core/DebugDraw.{h,cpp}` — shared producer/consumer line facility
- `samples/physics_smoke/jobsys_proof.cpp` — Phase-A 1000-body Tracy harness
- `samples/assets/scenes/drop_test.luth` — Tier 0 e2e smoke scene

**New (editor)**
- `luthien/source/luthien/inspectors/component_drawers/ColliderDrawer.cpp`
- `luthien/source/luthien/inspectors/component_drawers/RigidBodyDrawer.cpp`

**Modified**
- `dependencies.lua`, `premake5.lua`, `luth/premake5.lua` — Jolt include + AVX2 + link
- `luth/source/luth/core/App.cpp` — Jolt globals init/teardown, PhysicsSystem registration
- `luth/source/luth/core/EditorHooks.h` — `PhysicsDebugColorMode` enum + 10 physics-debug fields on `EditorViewportState`
- `luth/source/luth/scene/Components.h` — physics components umbrella
- `luth/source/luth/scene/Scene.cpp` — `~Scene` iterate-destroy via `ClearPreservingAssets`; `DuplicateEntity` copies physics components
- `luth/source/luth/scene/SceneSerializer.cpp` — read/write `Collider` + `RigidBody` + `motionQuality`
- `luth/source/luth/jobs/AtomicCounter.{h,cpp}` — public `Increment`/`Decrement`
- `luth/source/luth/jobs/JobSystem.cpp` — `WaitForCounter` yield-path tail fix (the UAF)
- `luth/source/luth/renderer/subsystems/DebugDrawSubsystem.cpp` — `cache.CurrentTag` before Allocate (GPU leak fix)
- `luth/source/luth/renderer/subsystems/EditorOverlaysSubsystem.{h,cpp}` — debug-draw render pass
- `luthien/source/luthien/EditorSettings.{h,cpp}` — physics-debug fields + JSON load/save
- `luthien/source/luthien/EditorHooks.cpp` — populate physics-debug fields in `GetViewportState`
- `luthien/source/luthien/panels/EditorSettingsWindow.cpp` — Physics section + 3×2 table
- `luthien/source/luthien/panels/ScenePanel.cpp` — gizmo-dropdown 3×2 table + saved-flags restore
- `luthien/source/luthien/commands/ComponentCommands.h` — `ComponentSnapshotCommand<T>` for union-member undo
- `luthien/source/luthien/inspectors/component_drawers/RegisterComponentDrawers.{h,cpp}` — register physics drawers

---

## Verification (Tier 0 smoke gate)

Smoke #1–#7 from the original Tier 0 spec, all green on `drop_test.luth` (Debug + Release):

1. **Drop** — dynamic box at y=10, `linearVelocity.y ≈ 0` (±1e-3) after 2 s
2. **Stack** — 10 unit boxes, top y > 8.5 after 5 s, no oscillation > 0.05
3. **Raycast** — deferred to Phase D
4. **Trigger** — deferred to Phase D (ContactListener)
5. **1000-body** — Tracy zones spread across ≥ 4 worker threads; step ≤ 4 ms median (passed in Phase-A harness)
6. **Serialize** — scene save → load round-trips Collider + RigidBody + motionQuality; bodies recreated; sim resumes
7. **Editor pause** — body y unchanged 60 frames in Editing; falls in Playing

Plus the wire-primitive smoke: 100 spheres in Release no longer drops to single-digit FPS; the Release `__report_gsfailure` is gone after the `WaitForCounter` UAF fix.
