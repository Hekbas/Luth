#include "luthpch.h"

#include "luth/physics/PhysicsListeners.h"

#include "luth/physics/JoltMath.h"

#include <Jolt/Physics/Body/Body.h>
#include <Jolt/Physics/Collision/ContactListener.h>
#include <Jolt/Physics/Collision/Shape/SubShapeIDPair.h>

namespace Luth::Physics
{
    u64 LuthContactListener::PackPairKey(JPH::BodyID a, JPH::BodyID b)
    {
        const u32 ai = a.GetIndexAndSequenceNumber();
        const u32 bi = b.GetIndexAndSequenceNumber();
        const u32 lo = ai < bi ? ai : bi;
        const u32 hi = ai < bi ? bi : ai;
        return (static_cast<u64>(hi) << 32) | static_cast<u64>(lo);
    }

    entt::entity LuthContactListener::ResolveEntity(JPH::BodyID id) const
    {
        const u32 idx = id.GetIndex();
        if (idx >= m_BodyIndex.size()) return entt::null;
        return m_BodyIndex[idx];
    }

    // ── Callbacks ──

    JPH::ValidateResult LuthContactListener::OnContactValidate(
        const JPH::Body& /*inBody1*/, const JPH::Body& /*inBody2*/,
        JPH::RVec3Arg /*inBaseOffset*/, const JPH::CollideShapeResult& /*inCollisionResult*/)
    {
        // Default accept; per-pair filtering already happens in ObjectLayerPairFilterImpl.
        return JPH::ValidateResult::AcceptAllContactsForThisBodyPair;
    }

    void LuthContactListener::OnContactAdded(const JPH::Body& b1, const JPH::Body& b2,
                                             const JPH::ContactManifold& manifold,
                                             JPH::ContactSettings& /*ioSettings*/)
    {
        // Body::IsSensor reads the flags bitfield with a lock-free atomic load (Body.h). Safe to
        // call from the multi-threaded JobFindCollisions phase. GetUserData is also lock-free.
        const bool isTrigger = b1.IsSensor() || b2.IsSensor();
        if (isTrigger)
        {
            EvaluateTrigger(b1.GetID(), b2.GetID(), /*isTrigger*/true);
        }
        else
        {
            EmitContactAdded(b1, b2, manifold);
        }
    }

    void LuthContactListener::OnContactPersisted(const JPH::Body& b1, const JPH::Body& b2,
                                                 const JPH::ContactManifold& /*manifold*/,
                                                 JPH::ContactSettings& /*ioSettings*/)
    {
        // Re-classify every step so Body::SetIsSensor flips (or monitorable-state changes once we
        // gain monitorable surfaces) produce the right Enter/Exit. We do NOT emit a ContactPersisted
        // gameplay event — Tier 0 ships enter/exit only.
        const bool isTrigger = b1.IsSensor() || b2.IsSensor();
        EvaluateTrigger(b1.GetID(), b2.GetID(), isTrigger);
    }

    void LuthContactListener::OnContactRemoved(const JPH::SubShapeIDPair& pair)
    {
        const auto a = pair.GetBody1ID();
        const auto b = pair.GetBody2ID();
        const u64  key = PackPairKey(a, b);

        bool wasTrigger;
        {
            SpinLockGuard g(m_CacheLock);
            wasTrigger = m_TriggerPairs.erase(key) > 0;
        }

        if (wasTrigger)
            EmitTriggerExit(a, b);
        else
            EmitContactRemoved(a, b);
    }

    // ── Cache evaluation ──

    void LuthContactListener::EvaluateTrigger(JPH::BodyID a, JPH::BodyID b, bool isTrigger)
    {
        const u64 key = PackPairKey(a, b);
        bool transitionEnter = false;
        bool transitionExit  = false;
        {
            SpinLockGuard g(m_CacheLock);
            if (isTrigger)
            {
                auto [it, inserted] = m_TriggerPairs.insert(key);
                transitionEnter = inserted;
            }
            else
            {
                transitionExit = m_TriggerPairs.erase(key) > 0;
            }
        }

        // V1: emit OUTSIDE the lock. TryPush is wait-free; holding SpinLock across it would inflate
        // the critical section without benefit since the cache is the source of truth — queue can
        // drop without desyncing state.
        if (transitionEnter)
            EmitTriggerEnter(a, b);
        else if (transitionExit)
            EmitTriggerExit(a, b);
    }

    // ── Emit helpers ──

    void LuthContactListener::EmitContactAdded(const JPH::Body& b1, const JPH::Body& b2,
                                               const JPH::ContactManifold& m)
    {
        PhysicsEvent ev{};
        ev.type    = PhysicsEventType::ContactAdded;
        ev.entityA = static_cast<entt::entity>(b1.GetUserData());
        ev.entityB = static_cast<entt::entity>(b2.GetUserData());

        // First contact point from the manifold is representative for Tier 0; full geometry would
        // require a vector<Vec3> or a span — defer until gameplay needs it.
        if (m.mRelativeContactPointsOn1.size() > 0)
        {
            const JPH::Vec3 pt = m.mBaseOffset + m.mRelativeContactPointsOn1[0];
            ev.worldPoint = FromJolt(pt);
        }
        ev.worldNormal      = FromJolt(m.mWorldSpaceNormal);
        ev.penetrationDepth = m.mPenetrationDepth;

        if (!m_Queue.TryPush(ev))
            m_OverflowCount.fetch_add(1, std::memory_order_relaxed);
    }

    void LuthContactListener::EmitContactRemoved(JPH::BodyID a, JPH::BodyID b)
    {
        PhysicsEvent ev{};
        ev.type    = PhysicsEventType::ContactRemoved;
        ev.entityA = ResolveEntity(a);
        ev.entityB = ResolveEntity(b);
        if (!m_Queue.TryPush(ev))
            m_OverflowCount.fetch_add(1, std::memory_order_relaxed);
    }

    void LuthContactListener::EmitTriggerEnter(JPH::BodyID a, JPH::BodyID b)
    {
        PhysicsEvent ev{};
        ev.type    = PhysicsEventType::TriggerEnter;
        ev.entityA = ResolveEntity(a);
        ev.entityB = ResolveEntity(b);
        if (!m_Queue.TryPush(ev))
            m_OverflowCount.fetch_add(1, std::memory_order_relaxed);
    }

    void LuthContactListener::EmitTriggerExit(JPH::BodyID a, JPH::BodyID b)
    {
        PhysicsEvent ev{};
        ev.type    = PhysicsEventType::TriggerExit;
        ev.entityA = ResolveEntity(a);
        ev.entityB = ResolveEntity(b);
        if (!m_Queue.TryPush(ev))
            m_OverflowCount.fetch_add(1, std::memory_order_relaxed);
    }
}
