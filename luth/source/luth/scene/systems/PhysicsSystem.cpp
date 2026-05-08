#include "luthpch.h"

#include "luth/scene/systems/PhysicsSystem.h"

#include "luth/scene/Scene.h"
#include "luth/scene/components/Common.h"
#include "luth/scene/components/Transform.h"
#include "luth/scene/components/Physics.h"
#include "luth/physics/JoltMath.h"
#include "luth/physics/ShapeBuilder.h"
#include "luth/core/EditorHooks.h"
#include "luth/core/diagnostics/Log.h"
#include "luth/core/diagnostics/Profiler.h"
#include "luth/core/time/Time.h"

#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Body/BodyInterface.h>
#include <Jolt/Physics/Body/MotionType.h>
#include <Jolt/Physics/EActivation.h>

#include <algorithm>

namespace Luth
{
    namespace
    {
        JPH::EMotionType ToJoltMotion(Component::RigidBody::Motion m)
        {
            switch (m)
            {
                case Component::RigidBody::Motion::Static:    return JPH::EMotionType::Static;
                case Component::RigidBody::Motion::Kinematic: return JPH::EMotionType::Kinematic;
                case Component::RigidBody::Motion::Dynamic:   return JPH::EMotionType::Dynamic;
            }
            return JPH::EMotionType::Static;
        }

        JPH::ObjectLayer PickLayer(const Component::RigidBody& rb)
        {
            if (rb.motion == Component::RigidBody::Motion::Static) return Physics::Layers::STATIC;
            if (rb.isSensor)                                       return Physics::Layers::TRIGGER;
            return Physics::Layers::MOVING;
        }
    }

    PhysicsSystem::PhysicsSystem()
        : m_TempAlloc(kTempAllocatorBytes)
        , m_JobAdapter(kAdapterMaxJobs, kAdapterMaxBarriers)
    {
        m_System.Init(kMaxBodies, kNumBodyMutexes, kMaxBodyPairs, kMaxContactConstraints,
                      m_BPLayers, m_OvBpFilter, m_LayerPairFilter);
        LH_CORE_INFO("PhysicsSystem initialized: {} max bodies, {} pairs, {} contacts",
                     kMaxBodies, kMaxBodyPairs, kMaxContactConstraints);
    }

    PhysicsSystem::~PhysicsSystem()
    {
        DetachSignals();

        // Destroy any remaining bodies before m_System tears down. Skipping this leaks the body
        // manager's slots but is otherwise safe — Jolt's destructor cleans up the body table itself.
        // Doing it explicitly keeps allocator stats honest.
        if (!m_BodyMap.empty())
        {
            auto& bi = m_System.GetBodyInterface();
            for (auto& [_, id] : m_BodyMap)
            {
                bi.RemoveBody(id);
                bi.DestroyBody(id);
            }
            m_BodyMap.clear();
        }
        m_PendingDestroy.clear();
    }

    // ── Update ──

    void PhysicsSystem::Update(Scene* scene)
    {
        LH_PROFILE_FUNCTION();

        // Signal plumbing and the deferred-destroy drain run regardless of PlayState. Bailing under
        // the Editing gate before connecting signals leaves component additions during scene load with
        // no listener — bodies never get built and the first Play has an empty world to step. Drain
        // also has to run so a destroy queued during Editing doesn't leak its Jolt body.
        EnsureSignalsConnected(scene);
        DrainPendingDestroys();

        // Editor gate. When no editor is registered (runtime-only build) Get() is null and the sim ticks
        // unconditionally. Paused + ConsumeStepRequest() to advance one step lands alongside body
        // lifecycle, when there is something visible to step.
        if (auto* hooks = EditorHooks::Get();
            hooks && hooks->GetPlayState() == PlayState::Editing)
        {
            m_Accumulator = 0.0f;
            return;
        }

        m_Accumulator += Time::DeltaTime();

        // Cap the accumulator so a frame stall doesn't spiral. Lost time is acceptable here — the alt is
        // an unbounded catch-up loop that makes a stall worse.
        const f32 maxAccum = kMaxSubSteps * kFixedDt;
        if (m_Accumulator > maxAccum)
        {
            LH_CORE_WARN("PhysicsSystem: accumulator overflow ({} ms), clamping", m_Accumulator * 1000.0f);
            m_Accumulator = maxAccum;
        }

        const int substeps = static_cast<int>(m_Accumulator / kFixedDt);
        if (substeps <= 0)
            return;

        // Kinematic bodies move once per frame at the user-set pose. Pre-step sync writes the target;
        // Jolt interpolates velocity over the substep block so collisions resolve as if the body moved
        // smoothly across the whole frame.
        SyncTransformsToBodies(scene);

        for (int i = 0; i < substeps; ++i)
        {
            Step(kFixedDt, /*collisionSteps*/1);
            m_Accumulator -= kFixedDt;
        }

        // Dynamic bodies write their post-step pose back to Transform / WorldTransform. Marking the
        // Transform dirty queues TransformSystem to rebuild LocalMatrix next frame.
        SyncBodiesToTransforms(scene);
    }

    void PhysicsSystem::Step(f32 dt, int collisionSteps)
    {
        LH_PROFILE_FUNCTION();
        m_System.Update(dt, collisionSteps, &m_TempAlloc, &m_JobAdapter);
    }

    // ── Signals ──

    void PhysicsSystem::EnsureSignalsConnected(Scene* scene)
    {
        if (!scene) return;
        auto* reg = &scene->Registry();
        if (reg == m_AttachedRegistry) return;

        // Scene change. Tear down old bodies and signals, then attach fresh.
        if (m_AttachedRegistry)
        {
            DetachSignals();
            if (!m_BodyMap.empty())
            {
                auto& bi = m_System.GetBodyInterface();
                for (auto& [_, id] : m_BodyMap)
                {
                    bi.RemoveBody(id);
                    bi.DestroyBody(id);
                }
                m_BodyMap.clear();
            }
            m_PendingDestroy.clear();
        }

        m_AttachedRegistry = reg;
        reg->on_construct<Component::Collider>().connect<&PhysicsSystem::OnComponentConstructed>(this);
        reg->on_construct<Component::RigidBody>().connect<&PhysicsSystem::OnComponentConstructed>(this);
        reg->on_destroy<Component::Collider>().connect<&PhysicsSystem::OnComponentDestroyed>(this);
        reg->on_destroy<Component::RigidBody>().connect<&PhysicsSystem::OnComponentDestroyed>(this);
    }

    void PhysicsSystem::DetachSignals()
    {
        if (!m_AttachedRegistry) return;
        auto* reg = m_AttachedRegistry;
        reg->on_construct<Component::Collider>().disconnect<&PhysicsSystem::OnComponentConstructed>(this);
        reg->on_construct<Component::RigidBody>().disconnect<&PhysicsSystem::OnComponentConstructed>(this);
        reg->on_destroy<Component::Collider>().disconnect<&PhysicsSystem::OnComponentDestroyed>(this);
        reg->on_destroy<Component::RigidBody>().disconnect<&PhysicsSystem::OnComponentDestroyed>(this);
        m_AttachedRegistry = nullptr;
    }

    void PhysicsSystem::OnComponentConstructed(entt::registry& reg, entt::entity entity)
    {
        // Either component arriving may complete a (Collider + RigidBody) pair. TryCreateBody is
        // idempotent — it bails if a body already exists or either component is still missing.
        TryCreateBody(reg, entity);
    }

    void PhysicsSystem::OnComponentDestroyed(entt::registry& reg, entt::entity entity)
    {
        // Either component leaving invalidates the pair; queue the body for deferred destruction so we
        // don't mutate Jolt state from inside an entity-destroy signal.
        DestroyBodyForEntity(reg, entity);
    }

    // ── Body lifecycle ──

    bool PhysicsSystem::TryCreateBody(entt::registry& reg, entt::entity entity)
    {
        if (m_BodyMap.contains(entity)) return false;
        if (!reg.all_of<Component::Collider, Component::RigidBody, Component::Transform>(entity))
            return false;

        const auto& collider  = reg.get<Component::Collider>(entity);
        const auto& rb        = reg.get<Component::RigidBody>(entity);
        const auto& transform = reg.get<Component::Transform>(entity);

        // Mesh / heightfield shapes are static-only in Jolt. Refuse the pairing rather than crash later.
        if ((collider.type == Component::Collider::Type::MeshRef) &&
            rb.motion != Component::RigidBody::Motion::Static)
        {
            LH_CORE_WARN("PhysicsSystem: MeshRef requires Static body — skipping entity {}",
                         (u32)entity);
            return false;
        }

        auto shape = Physics::BuildShape(collider);
        if (!shape)
        {
            // ConvexHullRef and MeshRef return null until the asset-shape cache lands; primitives
            // return null only on invalid params (already warned by ShapeBuilder).
            return false;
        }

        // Source the initial body pose from Transform (local) rather than WorldTransform: signals fire
        // during scene-load AddComponent calls, before TransformSystem has had a chance to propagate
        // local-to-world. Tier 0 contract is that physics-driven entities sit at the scene root, so
        // local == world here. Parented entities are warned below; their initial pose will be wrong
        // until TransformSystem catches up next frame.
        const Vec3 pos = transform.Position;
        const Quat rot = Quat(Math::Radians(transform.Rotation));

        if (reg.any_of<Component::Parent>(entity))
        {
            LH_CORE_WARN("PhysicsSystem: entity {} has a Parent — physics-driven Transform sync ignores"
                         " hierarchy and may drift", (u32)entity);
        }

        JPH::BodyCreationSettings bcs(shape, Physics::ToJolt(pos), Physics::ToJolt(rot),
                                      ToJoltMotion(rb.motion), PickLayer(rb));
        bcs.mIsSensor        = rb.isSensor;
        bcs.mLinearVelocity  = Physics::ToJolt(rb.linearVelocity);
        bcs.mAngularVelocity = Physics::ToJolt(rb.angularVelocity);
        bcs.mGravityFactor   = rb.gravityFactor;
        bcs.mLinearDamping   = rb.linearDamping;
        bcs.mAngularDamping  = rb.angularDamping;

        if (rb.mass > 0.0f)
        {
            bcs.mOverrideMassProperties        = JPH::EOverrideMassProperties::CalculateInertia;
            bcs.mMassPropertiesOverride.mMass  = rb.mass;
        }

        const auto activation = (rb.startActive && rb.motion != Component::RigidBody::Motion::Static)
                              ? JPH::EActivation::Activate : JPH::EActivation::DontActivate;

        auto& bi = m_System.GetBodyInterface();
        const JPH::BodyID id = bi.CreateAndAddBody(bcs, activation);
        if (id.IsInvalid())
        {
            LH_CORE_WARN("PhysicsSystem: CreateAndAddBody failed for entity {}", (u32)entity);
            return false;
        }

        // Pack entity into user data for reverse lookup from contact callbacks (when those land).
        bi.SetUserData(id, static_cast<JPH::uint64>(entity));

        m_BodyMap[entity] = id;
        auto& runtime = reg.get_or_emplace<Component::PhysicsBodyRuntime>(entity);
        runtime.bodyId = id.GetIndexAndSequenceNumber();
        runtime.shapeFingerprint = 0;
        return true;
    }

    void PhysicsSystem::DestroyBodyForEntity(entt::registry& /*reg*/, entt::entity entity)
    {
        auto it = m_BodyMap.find(entity);
        if (it == m_BodyMap.end()) return;

        // Queue + remove from m_BodyMap immediately so a paired on_destroy (e.g. RigidBody after
        // Collider) sees an empty entry and short-circuits.
        m_PendingDestroy.push_back({entity, it->second});
        m_BodyMap.erase(it);
    }

    void PhysicsSystem::DrainPendingDestroys()
    {
        if (m_PendingDestroy.empty()) return;

        auto& bi = m_System.GetBodyInterface();
        for (const auto& pd : m_PendingDestroy)
        {
            bi.RemoveBody(pd.bodyId);
            bi.DestroyBody(pd.bodyId);

            if (m_AttachedRegistry && m_AttachedRegistry->valid(pd.entity)
                && m_AttachedRegistry->any_of<Component::PhysicsBodyRuntime>(pd.entity))
            {
                m_AttachedRegistry->remove<Component::PhysicsBodyRuntime>(pd.entity);
            }
        }
        m_PendingDestroy.clear();
    }

    // ── Transform <-> Body sync ──

    void PhysicsSystem::SyncTransformsToBodies(Scene* scene)
    {
        LH_PROFILE_FUNCTION();
        if (!scene) return;

        auto& reg = scene->Registry();
        auto& bi  = m_System.GetBodyInterface();
        const f32 dt = kFixedDt;

        auto view = reg.view<Component::PhysicsBodyRuntime, Component::RigidBody, Component::WorldTransform>();
        for (auto entity : view)
        {
            const auto& rb = view.get<Component::RigidBody>(entity);
            if (rb.motion != Component::RigidBody::Motion::Kinematic) continue;

            const auto& runtime = view.get<Component::PhysicsBodyRuntime>(entity);
            if (runtime.bodyId == Component::PhysicsBodyRuntime::cInvalidBodyId) continue;

            const auto& world = view.get<Component::WorldTransform>(entity);
            Vec3 pos, scale;
            Quat rot;
            DecomposeTransform(world.Matrix, pos, rot, scale);

            bi.MoveKinematic(JPH::BodyID(runtime.bodyId),
                             Physics::ToJolt(pos), Physics::ToJolt(rot), dt);
        }
    }

    void PhysicsSystem::SyncBodiesToTransforms(Scene* scene)
    {
        LH_PROFILE_FUNCTION();
        if (!scene) return;

        auto& reg = scene->Registry();
        auto& bi  = m_System.GetBodyInterface();

        auto view = reg.view<Component::PhysicsBodyRuntime, Component::RigidBody,
                              Component::Transform, Component::WorldTransform>();
        for (auto entity : view)
        {
            const auto& rb = view.get<Component::RigidBody>(entity);
            if (rb.motion != Component::RigidBody::Motion::Dynamic) continue;

            const auto& runtime = view.get<Component::PhysicsBodyRuntime>(entity);
            if (runtime.bodyId == Component::PhysicsBodyRuntime::cInvalidBodyId) continue;

            const JPH::BodyID id(runtime.bodyId);
            const JPH::Vec3 pos = bi.GetPosition(id);
            const JPH::Quat rot = bi.GetRotation(id);
            const JPH::Vec3 lvel = bi.GetLinearVelocity(id);
            const JPH::Vec3 avel = bi.GetAngularVelocity(id);

            // Write back to local Transform — correct for root entities and the documented Tier 0
            // contract; parented entities drift (warned at body-create time). WorldTransform is also
            // updated directly so render systems reading it this frame see the post-step pose.
            auto& transform = view.get<Component::Transform>(entity);
            auto& world     = view.get<Component::WorldTransform>(entity);
            auto& rbMut     = const_cast<Component::RigidBody&>(rb);

            const Quat localRot = Physics::FromJolt(rot);
            transform.Position = Physics::FromJolt(pos);
            transform.Rotation = glm::eulerAngles(localRot) * Math::RadToDeg<f32>;
            transform.IsDirty  = true;

            world.Matrix = ComposeTransform(transform.Position, localRot, transform.Scale);

            rbMut.linearVelocity  = Physics::FromJolt(lvel);
            rbMut.angularVelocity = Physics::FromJolt(avel);
        }
    }
}
