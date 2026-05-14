#pragma once

#include "luth/core/types/LuthMath.h"

#include <entt/entt.hpp>

namespace Luth::Physics
{
    // The four events gameplay can react to. ContactAdded/Removed are emitted only for solid pairs
    // (neither body is a sensor); TriggerEnter/Exit are emitted when at least one body is a sensor.
    // ContactPersisted is intentionally not exposed — it fires every step for every active contact
    // (100s of fires per step for a stack) and gameplay rarely wants the noise. The listener still
    // processes Persisted internally so runtime sensor-flag flips and monitorable-state changes
    // produce the right Enter/Exit pair.
    enum class PhysicsEventType : u8
    {
        ContactAdded,
        ContactRemoved,
        TriggerEnter,
        TriggerExit,
    };

    // 40 bytes — fits in a cache line. Trigger and contact events share the same shape; the type
    // enum disambiguates. *Removed and *Exit events carry zero point/normal/penetration because
    // Jolt's OnContactRemoved callback doesn't provide manifold data — gameplay reads the entity
    // for state if needed.
    struct PhysicsEvent
    {
        PhysicsEventType type             = PhysicsEventType::ContactAdded;
        u8               _pad[3]          = {};
        entt::entity     entityA          = entt::null;
        entt::entity     entityB          = entt::null;
        Vec3             worldPoint       {0.0f};
        Vec3             worldNormal      {0.0f};
        f32              penetrationDepth = 0.0f;
    };

    static_assert(sizeof(PhysicsEvent) <= 64, "PhysicsEvent exceeds cache line");
}
