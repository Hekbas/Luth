#pragma once

#include "luth/core/types/LuthMath.h"
#include "luth/core/UUID.h"

// Forward declaration so CharacterControllerRuntime can name JPH::CharacterVirtual without
// pulling in the Jolt header. PhysicsSystem owns the heap allocation; this is a non-owning observer.
namespace JPH { class CharacterVirtual; }

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
        // layout; only the interpretation differs (convex hull vs triangle-mesh shape from the same
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
    // time. materialUUID points to a PhysicsMaterial asset; the zero UUID means engine default. Material is currently per-body;
    // when compound shapes land it moves per-shape.
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
    // serialized. The bodyId field is opaque (a JPH::BodyID's internal u32); gameplay code should not
    // read it directly. shapeFingerprint detects shape edits so the body can be rebuilt only when needed.
    struct PhysicsBodyRuntime
    {
        static constexpr u32 cInvalidBodyId = 0xffffffffu;

        u32 bodyId            = cInvalidBodyId;
        u64 shapeFingerprint  = 0;
    };

    // Ground state mirrors JPH::CharacterBase::EGroundState. Returned by CharacterVirtual after each
    // ExtendedUpdate and written back into CharacterController so gameplay can branch on it without
    // touching the JPH object.
    enum class GroundState : u8
    {
        OnGround,        // standing on a slope <= maxSlopeAngle
        OnSteepGround,   // contact, but slope > maxSlopeAngle (will slide)
        NotSupported,    // contact, but normal doesn't match Up axis (e.g. wall)
        InAir            // no ground contact
    };

    // Kinematic, query-driven character. Pairs with a Collider whose Type::Capsule supplies the swept
    // shape; TryCreateCharacter refuses non-capsule colliders (Failed + warn). Movement is fed via
    // desiredVelocity each frame (horizontal); PhysicsSystem integrates gravity into the y component
    // and consumes jumpQueued on grounded frames. groundState + currentVelocity are read-back fields
    // refreshed post-step. Mutually exclusive with RigidBody on the same entity.
    struct CharacterController
    {
        // Authoring (serialized). Defaults match JPH::CharacterVirtualSettings where applicable.
        f32 maxSlopeAngleDeg          = 45.0f;
        f32 mass                      = 70.0f;     // soft-body push at Tier 2 (currently informational)
        f32 maxStrength               = 100.0f;
        f32 characterPadding          = 0.02f;
        f32 predictiveContactDistance = 0.1f;
        f32 penetrationRecoverySpeed  = 1.0f;
        u8  layer                     = 1;         // Physics::Layers::MOVING (matches RigidBody convention)
        f32 gravityFactor             = 1.0f;
        f32 moveSpeed                 = 5.0f;      // consumed by PlayerControllerSystem stub
        f32 jumpSpeed                 = 6.0f;      // vertical kick on Jump() when grounded

        // Per-frame inputs (NOT serialized).
        Vec3 desiredVelocity {0.0f};
        bool jumpQueued      = false;

        // Read-back (NOT serialized; written by PhysicsSystem::UpdateCharacters each substep).
        GroundState groundState     = GroundState::InAir;
        Vec3        currentVelocity {0.0f};

        // API. Methods write to fields; PhysicsSystem reads them in the next Update.
        void SetDesiredVelocity(const Vec3& v) { desiredVelocity = v; }
        void Jump()                            { jumpQueued = true; }
        bool IsGrounded() const                { return groundState == GroundState::OnGround; }
    };

    // Non-owning observer into PhysicsSystem's character table. PhysicsSystem owns the heap-allocated
    // JPH::CharacterVirtual; this struct just exposes the pointer for debug-draw + queries and carries
    // a fingerprint so the slow-path rebuild can short-circuit when only tunable fields change.
    struct CharacterControllerRuntime
    {
        JPH::CharacterVirtual* character   = nullptr;
        u64                    fingerprint = 0;
    };
}
