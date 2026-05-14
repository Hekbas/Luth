#pragma once

#include "luth/core/types/LuthMath.h"

#include <Jolt/Jolt.h>
#include <Jolt/Physics/Body/BodyID.h>

#include <entt/entt.hpp>

namespace Luth::Physics
{
    // Result of PhysicsSystem::Raycast. fraction is along [0, 1] of the cast ray (origin + dir*maxDist);
    // distance is fraction * |dir*maxDist|. worldNormal points outward from the surface the ray hit.
    // entity is entt::null if the hit body has no entity binding (shouldn't happen for engine-managed
    // bodies; would happen if a Jolt body were created outside the EnTT lifecycle).
    struct RaycastHit
    {
        entt::entity entity      = entt::null;
        JPH::BodyID  bodyId;
        Vec3         worldPoint  {0.0f};
        Vec3         worldNormal {0.0f};
        f32          distance    = 0.0f;
        f32          fraction    = 0.0f;
    };

    // Result of PhysicsSystem::OverlapBox / OverlapSphere / OverlapCapsule. We surface only entity +
    // bodyId for Tier 0 — gameplay queries the entity for full state if it needs more. Penetration
    // depth and contact normal are available from Jolt's CollideShapeResult but adding them inflates
    // the per-hit cost (vector reads) without a clear consumer; revisit when a query consumer asks.
    struct OverlapHit
    {
        entt::entity entity = entt::null;
        JPH::BodyID  bodyId;
    };
}
