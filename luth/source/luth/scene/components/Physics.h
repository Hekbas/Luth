#pragma once

#include "luth/core/types/LuthMath.h"
#include "luth/core/UUID.h"

namespace Luth::Component
{
    // Shape attached to an entity. Inline parametric data for primitives; mesh/convex shapes reference a
    // Model asset via the meshRef union member. The tag enum drives interpretation; a tagged union (rather
    // than std::variant) keeps the component cache-line-tight. Compound shapes are deferred to a follow-up
    // tier and modeled as child entities each carrying their own Collider.
    struct Collider
    {
        enum class Type : u8
        {
            Box,
            Sphere,
            Capsule,
            ConvexHullRef,
            MeshRef
        };

        Type type = Type::Box;
        Vec3 localOffset{0.0f};
        Quat localRotation{1.0f, 0.0f, 0.0f, 0.0f};

        // The active union member is determined by `type`. ConvexHullRef and MeshRef share the meshRef
        // layout — only the interpretation differs (convex hull vs triangle-mesh shape from the same
        // model asset). modelHi/modelLo together form the asset's UUID; meshIndex selects the submesh.
        union
        {
            Vec3 boxHalfExtents;
            f32  sphereRadius;
            struct { f32 radius, halfHeight; } capsule;
            struct { u64 modelHi, modelLo; u32 meshIndex; } meshRef;
        };

        Collider() : boxHalfExtents(0.5f, 0.5f, 0.5f) {}
    };

    static_assert(sizeof(Collider) <= 64, "Collider exceeds 64-byte budget");

    // Body parameters and runtime velocity. mass <= 0 derives from shape volume * density at body build
    // time. materialUUID points to a PhysicsMaterial asset; the zero UUID means engine default. Per-body
    // material is a Tier 0 simplification — once compound shapes land, material moves per-shape.
    struct RigidBody
    {
        enum class Motion  : u8 { Static, Kinematic, Dynamic };
        // Discrete: cheap, but fast bodies tunnel through thin geometry. LinearCast: cast the shape
        // along its velocity each step, opt-in for projectiles or fast-fall objects.
        enum class Quality : u8 { Discrete, LinearCast };

        Motion  motion        = Motion::Dynamic;
        Quality motionQuality = Quality::Discrete;
        u8      layer         = 1;
        bool    isSensor      = false;
        bool    startActive   = true;
        f32     mass          = 0.0f;
        Vec3    linearVelocity{0.0f};
        Vec3    angularVelocity{0.0f};
        f32     gravityFactor   = 1.0f;
        f32     linearDamping   = 0.05f;
        f32     angularDamping  = 0.05f;
        UUID    materialUUID;
    };

    // Runtime-only handle into PhysicsSystem's body table. PhysicsSystem attaches this when it creates a
    // Jolt body for a (RigidBody + Collider) pair, and removes it when either component drops. Never
    // serialized. The bodyId field is opaque (a JPH::BodyID's internal u32) — gameplay code should not
    // read it directly. shapeFingerprint detects shape edits so the body can be rebuilt only when needed.
    struct PhysicsBodyRuntime
    {
        static constexpr u32 cInvalidBodyId = 0xffffffffu;

        u32 bodyId            = cInvalidBodyId;
        u64 shapeFingerprint  = 0;
    };
}
