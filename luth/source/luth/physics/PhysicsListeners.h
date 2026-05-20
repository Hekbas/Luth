#pragma once

#include "luth/core/types/LuthTypes.h"
#include "luth/jobs/MPMCQueue.h"
#include "luth/jobs/SpinLock.h"
#include "luth/physics/PhysicsEvents.h"

#include <Jolt/Jolt.h>
#include <Jolt/Physics/Body/BodyID.h>
#include <Jolt/Physics/Collision/ContactListener.h>

#include <entt/entt.hpp>

#include <atomic>
#include <span>
#include <unordered_set>

namespace Luth::Physics
{
    // Listener pattern (Godot jolt_contact_listener_3d.cpp reference): the trigger-pair cache lives
    // here, mutated under SpinLock from Jolt worker fibers. Cache state is the source of truth; the
    // event queue is dispatch only. If TryPush overflows, cache stays consistent and the next pair
    // Add self-heals — we just lose the gameplay-side notification for that overflow.
    //
    // Single-listener instance owned by PhysicsSystem. Body↔entity resolution uses two paths:
    // - OnContactAdded / OnContactPersisted: bodies are accessible, read via Body::GetUserData.
    // - OnContactRemoved: bodies are forbidden (ContactListener.h:127), read via the BodyIndex
    //   side table populated by PhysicsSystem at body-create / cleared at body-destroy.
    class LuthContactListener final : public JPH::ContactListener
    {
    public:
        // Bounded MPMC. 4096 events/frame is more than 10x the worst-case stack-of-100 we ship —
        // overflow drops events and is tracked via m_OverflowCount for once-per-frame warning at
        // drain time. Power-of-2 capacity is an MPMCQueue requirement.
        using EventQueue = MPMCQueue<PhysicsEvent, 4096>;

        LuthContactListener(EventQueue& queue, std::span<entt::entity> bodyIndex)
            : m_Queue(queue), m_BodyIndex(bodyIndex) {}

        JPH::ValidateResult OnContactValidate(const JPH::Body& inBody1, const JPH::Body& inBody2,
                                              JPH::RVec3Arg inBaseOffset,
                                              const JPH::CollideShapeResult& inCollisionResult) override;

        void OnContactAdded    (const JPH::Body& inBody1, const JPH::Body& inBody2,
                                const JPH::ContactManifold& inManifold,
                                JPH::ContactSettings& ioSettings) override;
        void OnContactPersisted(const JPH::Body& inBody1, const JPH::Body& inBody2,
                                const JPH::ContactManifold& inManifold,
                                JPH::ContactSettings& ioSettings) override;
        void OnContactRemoved  (const JPH::SubShapeIDPair& inSubShapePair) override;

        // Read+clear the overflow counter; called once per frame from PhysicsSystem::DrainEvents.
        u32 ConsumeOverflowCount() { return m_OverflowCount.exchange(0, std::memory_order_acq_rel); }

        // Diagnostic — for the v1 leak tripwire and post-Stop drain checks.
        size_t TriggerPairCount() const { return m_TriggerPairs.size(); }

    private:
        // Canonical pair key: lower BodyID's full 32-bit (index + sequence) in low half, higher
        // in high half. Sequence number prevents slot-reuse collisions (Jolt wraps it at 256;
        // pathologically aggressive churn could still collide — see plan §Risks). Storing in a
        // canonical order means OnContactRemoved doesn't have to try both orderings.
        static u64 PackPairKey(JPH::BodyID a, JPH::BodyID b);

        // Shared trigger-classification path called from OnContactAdded AND OnContactPersisted.
        // Persisted re-runs the evaluation each step so runtime Body::SetIsSensor flips produce
        // the matching Enter/Exit (without it, sensor flips during overlap silently miss).
        void EvaluateTrigger(JPH::BodyID a, JPH::BodyID b, bool isTrigger);

        entt::entity ResolveEntity(JPH::BodyID id) const;

        void EmitContactAdded  (const JPH::Body& b1, const JPH::Body& b2, const JPH::ContactManifold& m);
        void EmitContactRemoved(JPH::BodyID a, JPH::BodyID b);
        void EmitTriggerEnter  (JPH::BodyID a, JPH::BodyID b);
        void EmitTriggerExit   (JPH::BodyID a, JPH::BodyID b);

        EventQueue&             m_Queue;
        std::span<entt::entity> m_BodyIndex;       // BodyID.GetIndex() -> entt::entity
        SpinLock                m_CacheLock;
        std::unordered_set<u64> m_TriggerPairs;    // cross-frame trigger-pair source of truth
        std::atomic<u32>        m_OverflowCount{0};
    };
}
