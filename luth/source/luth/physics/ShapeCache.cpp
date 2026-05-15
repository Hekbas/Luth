#include "luthpch.h"

#include "luth/physics/ShapeCache.h"

#include "luth/physics/ShapeBuilder.h"
#include "luth/scene/components/Physics.h"

namespace Luth::Physics
{
    BuildOutcome ShapeCache::GetOrBuild(const Component::Collider& c)
    {
        using Type = Component::Collider::Type;
        switch (c.type)
        {
            case Type::Box:
            case Type::Sphere:
            case Type::Capsule:
                // One shape per body — caching primitives would cost a lookup per build for no
                // sharing benefit. Delegate inline.
                return { Physics::BuildShape(c), false };

            case Type::ConvexHullRef:
            case Type::MeshRef:
                // Asset-backed lazy build lands in the next sub-task; today both kinds short-circuit
                // to nullptr with retryLater=false so the entity is dropped from the build queue
                // (matches pre-Phase-E behaviour where ShapeBuilder returned null for these types).
                return { nullptr, false };
        }
        return { nullptr, false };
    }

    void ShapeCache::OnAssetReimported(std::span<const UUID> dirtyUUIDs)
    {
        if (dirtyUUIDs.empty()) return;
        SpinLockGuard guard(m_Lock);
        for (auto it = m_Map.begin(); it != m_Map.end(); )
        {
            const UUID model(it->first.modelHi, it->first.modelLo);
            bool match = false;
            for (const UUID& d : dirtyUUIDs) { if (d == model) { match = true; break; } }
            if (match) it = m_Map.erase(it);
            else       ++it;
        }
    }

    void ShapeCache::Invalidate(UUID uuid)
    {
        const UUID one[1] = { uuid };
        OnAssetReimported(std::span<const UUID>(one, 1));
    }

    void ShapeCache::Clear()
    {
        SpinLockGuard guard(m_Lock);
        m_Map.clear();
    }

    u64 ShapeCache::ComputeFingerprint(const Component::Collider& /*c*/)
    {
        // Populated in the fingerprint sub-task. Returning 0 here matches the legacy code path
        // (PhysicsBodyRuntime::shapeFingerprint was always 0) so behaviour is unchanged.
        return 0;
    }
}
