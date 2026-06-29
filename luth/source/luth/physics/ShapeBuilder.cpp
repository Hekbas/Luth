#include "luthpch.h"

#include "luth/physics/ShapeBuilder.h"

#include "luth/physics/JoltMath.h"
#include "luth/scene/components/Physics.h"
#include "luth/core/diagnostics/Log.h"

#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <Jolt/Physics/Collision/Shape/SphereShape.h>
#include <Jolt/Physics/Collision/Shape/CapsuleShape.h>
#include <Jolt/Physics/Collision/Shape/RotatedTranslatedShape.h>

namespace Luth::Physics
{
    namespace
    {
        // Build the inner (un-offset, un-rotated) shape from the active union member. ConvexHullRef and
        // MeshRef return nullptr — they need an asset-side shape cache that lands later.
        JPH::RefConst<JPH::Shape> BuildPrimitive(const Component::Collider& c)
        {
            using Type = Component::Collider::Type;
            switch (c.type)
            {
                case Type::Box:
                {
                    if (c.boxHalfExtents.x <= 0.0f || c.boxHalfExtents.y <= 0.0f || c.boxHalfExtents.z <= 0.0f)
                    {
                        LH_LOG(Physics, warn, "ShapeBuilder: Box halfExtents must be positive (got {}, {}, {})",
                                     c.boxHalfExtents.x, c.boxHalfExtents.y, c.boxHalfExtents.z);
                        return nullptr;
                    }
                    JPH::BoxShapeSettings settings(ToJolt(c.boxHalfExtents));
                    auto result = settings.Create();
                    return result.IsValid() ? result.Get() : nullptr;
                }

                case Type::Sphere:
                {
                    if (c.sphereRadius <= 0.0f)
                    {
                        LH_LOG(Physics, warn, "ShapeBuilder: Sphere radius must be positive (got {})", c.sphereRadius);
                        return nullptr;
                    }
                    JPH::SphereShapeSettings settings(c.sphereRadius);
                    auto result = settings.Create();
                    return result.IsValid() ? result.Get() : nullptr;
                }

                case Type::Capsule:
                {
                    if (c.capsule.radius <= 0.0f || c.capsule.halfHeight <= 0.0f)
                    {
                        LH_LOG(Physics, warn, "ShapeBuilder: Capsule radius/halfHeight must be positive (got {}, {})",
                                     c.capsule.radius, c.capsule.halfHeight);
                        return nullptr;
                    }
                    JPH::CapsuleShapeSettings settings(c.capsule.halfHeight, c.capsule.radius);
                    auto result = settings.Create();
                    return result.IsValid() ? result.Get() : nullptr;
                }

                case Type::ConvexHullRef:
                case Type::MeshRef:
                    // Asset-backed shapes route through Physics::ShapeCache (UUID-keyed lazy build
                    // + reimport invalidation). This builder only knows about primitives; callers
                    // reaching the asset cases here get nullptr and fall through to the cache path.
                    return nullptr;
            }
            return nullptr;
        }

        bool IsZero(const Vec3& v)
        {
            return v.x == 0.0f && v.y == 0.0f && v.z == 0.0f;
        }

        bool IsIdentity(const Quat& q)
        {
            // glm::quat constructor is (w, x, y, z); identity has w == 1, xyz == 0.
            return q.w == 1.0f && q.x == 0.0f && q.y == 0.0f && q.z == 0.0f;
        }
    }

    JPH::RefConst<JPH::Shape> BuildShape(const Component::Collider& c)
    {
        JPH::RefConst<JPH::Shape> inner = BuildPrimitive(c);
        if (!inner) return nullptr;

        if (IsZero(c.localOffset) && IsIdentity(c.localRotation))
            return inner;

        JPH::RotatedTranslatedShapeSettings wrap(ToJolt(c.localOffset), ToJolt(c.localRotation), inner);
        auto result = wrap.Create();
        return result.IsValid() ? result.Get() : inner;
    }
}
