#pragma once

#include "luth/scene/systems/ISystem.h"
#include "luth/core/UUID.h"
#include "luth/jobs/SpinLock.h"
#include "luth/physics/PhysicsEvents.h"
#include "luth/physics/PhysicsLayers.h"
#include "luth/physics/PhysicsListeners.h"
#include "luth/physics/PhysicsQuery.h"
#include "luth/physics/LuthJobSystemForJolt.h"
#include "luth/physics/PhysicsDebugRenderer.h"
#include "luth/physics/ShapeCache.h"

#include <Jolt/Jolt.h>

#include <Jolt/Core/TempAllocator.h>
#include <Jolt/Physics/Body/BodyID.h>
#include <Jolt/Physics/PhysicsSystem.h>

#include <entt/entt.hpp>
#include <atomic>
#include <span>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace JPH { class Shape; class CharacterVirtual; }

namespace Luth
{
    class Scene;
    namespace Component { struct CharacterController; }

    // Drives the JPH::PhysicsSystem instance for the active scene. Owns the broadphase / object layer
    // filters, the LuthJobSystemForJolt adapter, and the temp allocator. Connects EnTT signals on the
    // scene's registry to keep Jolt bodies in lock-step with (Collider + RigidBody) component pairs:
    // construct fires create, destroy queues a deferred remove. Update() drains the destroy queue,
    // syncs kinematic Transforms into bodies, runs the fixed-step accumulator, then syncs dynamic body
    // poses back into Transforms.
    class PhysicsSystem final : public ISystem
    {
    public:
        PhysicsSystem();
        ~PhysicsSystem() override;

        void Update(Scene* scene) override;

        // Collision queries. Safe to call between Update()s — NOT during Step() (Jolt's
        // NarrowPhaseQuery holds body locks while the broadphase mutates). Re-entry asserts.
        //
        // layerMask is a bitmask of (1u << Layers::X) values; pass 0 to skip layer filtering.
        // Raycast returns false on miss with outHit untouched; on hit, fraction is [0, 1] along
        // the directed segment origin + dir*maxDist, distance is fraction * maxDist.
        bool Raycast(const Vec3& origin, const Vec3& dir, f32 maxDist,
                     u32 layerMask, Physics::RaycastHit& outHit) const;

        // Find every body whose shape overlaps the given primitive. Hits are written into outHits
        // up to its size; the return value is the actual count (so callers can detect "clamped"
        // by comparing return == span.size()). Order is broadphase-walk order, not sorted by
        // distance. rot for Sphere is meaningless (still in the signature for symmetry — pass
        // identity).
        u32 OverlapBox    (const Vec3& center, const Vec3& halfExtents, const Quat& rot,
                           u32 layerMask, std::span<Physics::OverlapHit> outHits) const;
        u32 OverlapSphere (const Vec3& center, f32 radius,
                           u32 layerMask, std::span<Physics::OverlapHit> outHits) const;
        u32 OverlapCapsule(const Vec3& center, f32 radius, f32 halfHeight, const Quat& rot,
                           u32 layerMask, std::span<Physics::OverlapHit> outHits) const;

        // Drain physics events generated during the most recent Step into the caller's span.
        // Returns the count written (clamped to outEvents.size()). Pumping events without a buffer
        // (outEvents.empty()) is a no-op. Call once per frame after PhysicsSystem::Update returns.
        u32 DrainEvents(std::span<Physics::PhysicsEvent> outEvents);

    private:
        // Shared core for the three overlap overloads. Caller hands over an already-built JPH
        // shape (Ref so it lives until the call returns) plus the world COM transform; we wire
        // up the layer-mask filters, run CollideShape, and write hits into outHits clamped.
        u32 OverlapShape(const JPH::Shape* shape, const Vec3& center, const Quat& rot,
                         u32 layerMask, std::span<Physics::OverlapHit> outHits) const;

        struct PendingDestroy
        {
            entt::entity entity;
            JPH::BodyID  bodyId;
        };

        // Character lifetime is owned here (heap-allocated JPH::CharacterVirtual) so the destroy queue
        // carries the pointer directly — by the time we drain we may have lost the entity from the
        // registry. Mirror of PendingDestroy for the character path.
        struct PendingCharacterDestroy
        {
            entt::entity           entity;
            JPH::CharacterVirtual* character;
        };

        // Result of a single TryCreateBody attempt. RetryLater means a transient miss (asset not
        // loaded yet) — DrainPendingBuilds collects these into a shadow vector and re-queues them
        // for next Update. Failed is permanent (missing components, invalid shape, opt-out): the
        // entity is dropped from the queue. Created is the success path.
        enum class BuildResult { Created, RetryLater, Failed };

        // Lazily attaches signal handlers to the scene's registry on the first Update with a non-null
        // scene, and on every subsequent scene change. Detach happens in the destructor.
        void EnsureSignalsConnected(Scene* scene);
        void DetachSignals();

        // EnTT signal targets. on_construct + on_update on either Collider or RigidBody route to
        // OnComponentConstructed / OnComponentUpdated, both of which queue the entity for a deferred
        // body (re)build drained at the start of the next Update — that way Inspector add-then-edit
        // and shape changes during Play both produce a body that matches the latest fields. on_destroy
        // routes to DestroyBodyForEntity; either component leaving invalidates the pair.
        void OnComponentConstructed(entt::registry& reg, entt::entity entity);
        void OnComponentUpdated(entt::registry& reg, entt::entity entity);
        void OnComponentDestroyed(entt::registry& reg, entt::entity entity);

        BuildResult TryCreateBody(Scene* scene, entt::entity entity);
        void DestroyBodyForEntity(entt::registry& reg, entt::entity entity);
        void DrainPendingDestroys();

        // Character lifecycle. Mirrors the body path: TryCreateCharacter validates the (Collider Capsule
        // + CharacterController + Transform) tuple, builds a CharacterVirtual, attaches the runtime
        // component. Destroy is two-phase to keep deletions off the signal-callback thread, same as
        // bodies. ApplyCharacterTuning is the fast path for fingerprint-match drains.
        BuildResult TryCreateCharacter(Scene* scene, entt::entity entity);
        void DestroyCharacterForEntity(entt::registry& reg, entt::entity entity);
        void DrainPendingCharacterDestroys();
        void ApplyCharacterTuning(JPH::CharacterVirtual* ch, const Component::CharacterController& cc);

        // Dedup-push to m_PendingBuild. Inline scan keeps cost negligible for the small queue sizes
        // expected (one entry per edited entity per frame).
        void QueueBuild(entt::entity entity);
        void DrainPendingBuilds(Scene* scene);

        // Snapshot the dirty-UUID scratch under SpinLock, invalidate matching cache entries, then
        // walk the registry to push entities whose colliders reference the dirty UUIDs onto
        // m_PendingBuild. Runs on the game-stage fiber at the top of Update so callbacks staged
        // from the App-loop thread reach the build queue without crossing the SpinLock for the
        // expensive registry walk.
        void DrainDirtyAssets(Scene* scene);

        // Subscribe once to AssetDatabase::AddChangeCallback. The callback is bound to `this` and
        // only pushes UUIDs into m_DirtyAssetsScratch — registry walks happen on the game-stage
        // fiber. PhysicsSystem outlives the App loop so unregister is unnecessary (and unsupported
        // by AssetDatabase today).
        void EnsureChangeCallbackRegistered();

        // Fast-path field tuning when DrainPendingBuilds detects a fingerprint match — only
        // damping / gravity factor / linear-angular velocity are applied here. Mass and structural
        // fields (motion, layer, sensor, motionQuality, shape) live in the fingerprint and force
        // a full rebuild when they change.
        void ApplyRigidBodyTuning(JPH::BodyID id, const Component::RigidBody& rb);

        void SyncTransformsToBodies(Scene* scene);
        void SyncBodiesToTransforms(Scene* scene);

        // Substep character update. Runs inside the m_StepInFlight guard between body sync and the
        // JPH world step. Integrates gravity into velocity (JPH does not — see CharacterVirtual.h:322),
        // consumes jumpQueued on grounded frames, calls ExtendedUpdate (default StickToFloor /
        // WalkStairs settings), then writes groundState + currentVelocity back to the component.
        void UpdateCharacters(Scene* scene, f32 dt);

        // Post-step pose write-back. Character::GetPosition is authoritative; flip Transform.IsDirty
        // so TransformSystem rebuilds the local + world matrices next frame.
        void SyncCharactersToTransforms(Scene* scene);

        void Step(f32 fixedDt, int collisionSteps);

        // Walks the EnTT registry and emits wire primitives for each body's collider, AABB, and
        // centre-of-mass via the shared DebugDraw facility. Three independent passes (Shapes /
        // AABBs / CoM); each gated by a (Selected / All) pair from EditorViewportState. Runs every
        // frame regardless of PlayState so colliders stay visible while authoring (Editing).
        // Skipped entirely when no editor hook is registered (runtime build).
        void DrawDebugBodies(Scene* scene);

        // Order matters: m_TempAlloc and m_JobAdapter are referenced by m_System.Update() each step,
        // and member destruction is reverse-declaration order. Putting m_System last ensures it tears
        // down before its dependencies — and m_BodyMap before m_System so any leftover BodyID lookups
        // during teardown stay valid.
        Physics::BPLayerInterfaceImpl              m_BPLayers;
        Physics::ObjectVsBroadPhaseLayerFilterImpl m_OvBpFilter;
        Physics::ObjectLayerPairFilterImpl         m_LayerPairFilter;
        JPH::TempAllocatorImpl                     m_TempAlloc;
        Physics::LuthJobSystemForJolt              m_JobAdapter;
        JPH::PhysicsSystem                         m_System;

        std::unordered_map<entt::entity, JPH::BodyID> m_BodyMap;
        std::vector<PendingDestroy>                   m_PendingDestroy;
        std::vector<entt::entity>                     m_PendingBuild;

        // Character bookkeeping. m_CharacterMap owns the heap-allocated CharacterVirtual; the entity's
        // CharacterControllerRuntime holds a non-owning observer pointer + fingerprint. PendingCharacter-
        // Destroy carries both because the entity may be gone by drain time. m_WarnedNonCapsule guards
        // the one-shot warning when a CharacterController is paired with a non-capsule Collider.
        std::unordered_map<entt::entity, JPH::CharacterVirtual*> m_CharacterMap;
        std::vector<PendingCharacterDestroy>                     m_PendingCharacterDestroy;
        std::unordered_set<entt::entity>                         m_WarnedNonCapsule;
        std::unordered_set<entt::entity>                         m_WarnedBothComponents;
        entt::registry*                               m_AttachedRegistry = nullptr;
        Scene*                                        m_AttachedScene    = nullptr;

        // Asset-backed shape cache. Cleared on scene change; entries invalidated on reimport via
        // OnAssetReimported (driven from DrainDirtyAssets).
        Physics::ShapeCache                           m_ShapeCache;

        // Hot-reload plumbing. The change callback (App-loop thread) pushes UUIDs into the scratch
        // under SpinLock; the game-stage fiber drains it at Update start. Lock contention is rare
        // (FileWatcher polls at 1 Hz) and the critical section is a vector::insert, so SpinLock fits
        // V1 (sub-microsecond hold time, no fiber yield inside).
        SpinLock                                      m_DirtyAssetsLock;
        std::vector<UUID>                             m_DirtyAssetsScratch;
        bool                                          m_ChangeCallbackRegistered = false;

        // Body-index reverse table for OnContactRemoved (where Jolt forbids body access). Sized
        // kMaxBodies at ctor; entries set at TryCreateBody alongside SetUserData, cleared at body
        // destroy. Slot reuse is safe across frames — BodyID's sequence number (8-bit) makes the
        // packed cache key unique per allocation. The Listener reads this lock-free; main-thread
        // writes are paired with body lifecycle events that don't race against contact callbacks.
        std::vector<entt::entity>            m_EntityByBodyIndex;

        // Event queue + listener. Listener writes from worker fibers under SpinLock for the trigger
        // cache; gameplay drains via DrainEvents on the main fiber. Order in declaration matters:
        // m_Queue and m_EntityByBodyIndex must outlive m_ContactListener so its constructor refs
        // are valid through teardown.
        Physics::LuthContactListener::EventQueue m_Queue;
        Physics::LuthContactListener             m_ContactListener;

#ifdef JPH_DEBUG_RENDERER
        Physics::PhysicsDebugRenderer                 m_DebugRenderer;
#endif

        // Set inside Step() around m_System.Update so queries can assert against re-entry.
        // acquire/release on both ends gives the assertion its happens-before edge against the
        // simulation's body-lock acquisitions.
        std::atomic<bool> m_StepInFlight{false};

        f32 m_Accumulator = 0.0f;

        static constexpr f32 kFixedDt     = 1.0f / 60.0f;
        static constexpr int kMaxSubSteps = 4;

        static constexpr JPH::uint kMaxBodies              = 16384;
        static constexpr JPH::uint kNumBodyMutexes         = 0;
        static constexpr JPH::uint kMaxBodyPairs           = 65536;
        static constexpr JPH::uint kMaxContactConstraints  = 16384;

        static constexpr JPH::uint kTempAllocatorBytes     = 32 * 1024 * 1024;
        static constexpr JPH::uint kAdapterMaxJobs         = 2048;
        static constexpr JPH::uint kAdapterMaxBarriers     = 8;
    };
}
