#pragma once

#include "luth/core/UUID.h"
#include "luth/core/types/LuthTypes.h"
#include "luth/jobs/SpinLock.h"

#include <Jolt/Jolt.h>
#include <Jolt/Physics/Collision/Shape/Shape.h>

#include <span>
#include <unordered_map>

namespace Luth::Component { struct Collider; struct RigidBody; }

namespace Luth::Physics
{
    // Cache key for asset-backed shapes. Identity is (model UUID, sub-mesh index, shape kind) so the
    // same source mesh can produce both a ConvexHullShape and a MeshShape entry without collision.
    // Primitives bypass the cache — one body, one shape; the lookup cost beats the build cost.
    struct ShapeKey
    {
        u64 modelHi  = 0;
        u64 modelLo  = 0;
        u32 meshIdx  = 0;
        u8  kind     = 0;
        u8  _pad[3]  = {0, 0, 0};

        bool operator==(const ShapeKey& other) const noexcept
        {
            return modelHi == other.modelHi && modelLo == other.modelLo
                && meshIdx == other.meshIdx && kind == other.kind;
        }
    };

    struct ShapeKeyHash
    {
        size_t operator()(const ShapeKey& k) const noexcept
        {
            // FNV-1a over the eight load-bearing bytes. Pad bytes are zero by construction.
            u64 h = 1469598103934665603ull;
            auto mix = [&h](u64 v) {
                for (int i = 0; i < 8; ++i) { h ^= (v >> (i * 8)) & 0xff; h *= 1099511628211ull; }
            };
            mix(k.modelHi);
            mix(k.modelLo);
            mix(static_cast<u64>(k.meshIdx) | (static_cast<u64>(k.kind) << 32));
            return static_cast<size_t>(h);
        }
    };

    // Outcome of a shape build request. shape == nullptr means no body should be created this frame;
    // retryLater discriminates a transient miss (asset still streaming in — try again next Update)
    // from a permanent failure (bad params, opt-out, invalid UUID — drop the request).
    struct BuildOutcome
    {
        JPH::RefConst<JPH::Shape> shape;
        bool retryLater = false;
    };

    // Per-PhysicsSystem cache of asset-backed JPH::Shape refs. Reads from the body-build path on the
    // game-stage fiber; writes when a model finishes loading or a reimport invalidates an entry.
    //
    // Hot-reload: AssetDatabase::ChangeCallback fires from whatever thread runs the App loop's
    // ProcessPendingChanges; PhysicsSystem stages dirty UUIDs into a scratch vector under SpinLock,
    // then drains it at the start of its own Update — so OnAssetReimported here always runs on the
    // game-stage fiber. Erase releases this cache's RefConst; in-flight bodies still holding the old
    // shape keep it alive until their rebuild drops the reference.
    //
    // invariant: never call from inside JPH::PhysicsSystem::Update — see PhysicsSystem::Step's
    // re-entry guard for why the body-build path itself is gated.
    class ShapeCache
    {
    public:
        // Build or fetch a shape. Primitives skip the map and call into ShapeBuilder inline; asset-
        // backed kinds resolve through the SpinLock-guarded map keyed by (uuid, meshIdx, kind).
        BuildOutcome GetOrBuild(const Component::Collider& collider);

        // Drop entries matching any of these UUIDs. The caller is responsible for re-queueing the
        // affected entities so their bodies pick up the rebuilt shapes — this method only touches
        // the cache.
        void OnAssetReimported(std::span<const UUID> dirtyUUIDs);

        // Single-UUID convenience for direct invalidation (rare; mostly for testing).
        void Invalidate(UUID uuid);

        // Drop everything. Called on scene change so a fresh scene can't reuse a previous scene's
        // shapes — model UUIDs may collide across projects after UnloadProject + LoadProject.
        void Clear();

        // Stable hash of the structural fields that drive body construction. Stored on
        // PhysicsBodyRuntime so the deferred-build queue can skip rebuild when only tunable
        // RigidBody fields changed (damping, gravityFactor, velocity). Inputs:
        //   - Collider type + active union member + localOffset + localRotation
        //   - RigidBody motion + layer + isSensor + motionQuality + mass
        // Damping/gravity/velocity are intentionally NOT in the hash — those route through the
        // ApplyRigidBodyTuning fast path instead.
        static u64 ComputeFingerprint(const Component::Collider& collider,
                                      const Component::RigidBody& rb);

    private:
        SpinLock m_Lock;
        std::unordered_map<ShapeKey, JPH::RefConst<JPH::Shape>, ShapeKeyHash> m_Map;
    };
}
