#pragma once

#include <Jolt/Jolt.h>
#include <Jolt/Physics/Collision/Shape/Shape.h>

namespace Luth::Component { struct Collider; }

namespace Luth::Physics
{
    // Builds a Jolt shape from a primitive Collider component (Box/Sphere/Capsule). Returns nullptr
    // for ConvexHullRef / MeshRef (those route through Physics::ShapeCache for asset-backed lazy
    // build + reimport invalidation) or for invalid parameters. The returned ref is the caller's;
    // pass it directly into BodyCreationSettings.
    //
    // localOffset / localRotation on the Collider are applied by wrapping the inner primitive in a
    // RotatedTranslatedShape; identity offsets and rotations skip the wrap so the inner shape is
    // returned directly (one less indirection during collision dispatch).
    JPH::RefConst<JPH::Shape> BuildShape(const Component::Collider& collider);
}
