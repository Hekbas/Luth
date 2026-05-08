#pragma once

#include "luth/scene/systems/ISystem.h"
#include "luth/physics/PhysicsLayers.h"
#include "luth/physics/LuthJobSystemForJolt.h"

#include <Jolt/Jolt.h>

#include <Jolt/Core/TempAllocator.h>
#include <Jolt/Physics/Body/BodyID.h>
#include <Jolt/Physics/PhysicsSystem.h>

#include <entt/entt.hpp>
#include <unordered_map>
#include <vector>

namespace Luth
{
    class Scene;

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

    private:
        struct PendingDestroy
        {
            entt::entity entity;
            JPH::BodyID  bodyId;
        };

        // Lazily attaches signal handlers to the scene's registry on the first Update with a non-null
        // scene, and on every subsequent scene change. Detach happens in the destructor.
        void EnsureSignalsConnected(Scene* scene);
        void DetachSignals();

        // EnTT signal targets. on_construct on either Collider or RigidBody routes here; the body is
        // built only when both components plus a WorldTransform are present and no body exists yet.
        // on_destroy routes to DestroyBodyForEntity; either component leaving invalidates the pair.
        void OnComponentConstructed(entt::registry& reg, entt::entity entity);
        void OnComponentDestroyed(entt::registry& reg, entt::entity entity);

        bool TryCreateBody(entt::registry& reg, entt::entity entity);
        void DestroyBodyForEntity(entt::registry& reg, entt::entity entity);
        void DrainPendingDestroys();

        void SyncTransformsToBodies(Scene* scene);
        void SyncBodiesToTransforms(Scene* scene);
        void Step(f32 fixedDt, int collisionSteps);

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
        entt::registry*                               m_AttachedRegistry = nullptr;

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
