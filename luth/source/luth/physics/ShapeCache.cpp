#include "luthpch.h"

#include "luth/physics/ShapeCache.h"

#include "luth/physics/ShapeBuilder.h"
#include "luth/physics/JoltMath.h"
#include "luth/scene/components/Physics.h"
#include "luth/renderer/resources/Model.h"
#include "luth/resources/AssetManager.h"
#include "luth/core/diagnostics/Log.h"

#include <Jolt/Physics/Collision/Shape/ConvexHullShape.h>
#include <Jolt/Physics/Collision/Shape/MeshShape.h>
#include <Jolt/Physics/Collision/Shape/RotatedTranslatedShape.h>

namespace Luth::Physics
{
    namespace
    {
        bool IsZero(const Vec3& v)
        {
            return v.x == 0.0f && v.y == 0.0f && v.z == 0.0f;
        }

        bool IsIdentity(const Quat& q)
        {
            return q.w == 1.0f && q.x == 0.0f && q.y == 0.0f && q.z == 0.0f;
        }

        // Wrap an inner shape if either offset or rotation is non-identity. Returns the inner shape
        // directly when both are identity so the dispatch path stays one indirection shallower.
        // Mirrors ShapeBuilder.cpp's tail; duplicated here to keep ShapeBuilder primitive-only.
        JPH::RefConst<JPH::Shape> WrapWithOffset(JPH::RefConst<JPH::Shape> inner,
                                                 const Vec3& off, const Quat& rot)
        {
            if (IsZero(off) && IsIdentity(rot)) return inner;
            JPH::RotatedTranslatedShapeSettings wrap(Physics::ToJolt(off), Physics::ToJolt(rot), inner);
            auto result = wrap.Create();
            return result.IsValid() ? result.Get() : inner;
        }

        // Build the un-wrapped inner shape from one mesh's vertex/index data. ConvexHull only needs
        // positions; MeshShape needs positions + indices. Skinned-vertex meshes use SkinnedVertices
        // (positions are at the same field offset but the strides differ — branch and copy).
        JPH::RefConst<JPH::Shape> BuildAssetShape(Component::Collider::Type kind,
                                                  const MeshData& mesh)
        {
            using Type = Component::Collider::Type;
            if (kind == Type::ConvexHullRef)
            {
                std::vector<JPH::Vec3> points;
                if (mesh.IsSkinned)
                {
                    points.reserve(mesh.SkinnedVertices.size());
                    for (const auto& v : mesh.SkinnedVertices)
                        points.push_back(Physics::ToJolt(v.Position));
                }
                else
                {
                    points.reserve(mesh.Vertices.size());
                    for (const auto& v : mesh.Vertices)
                        points.push_back(Physics::ToJolt(v.Position));
                }
                if (points.size() < 4)
                {
                    LH_CORE_WARN("ShapeCache: ConvexHull needs >=4 points (mesh '{}' has {})",
                                 mesh.Name, points.size());
                    return nullptr;
                }
                JPH::ConvexHullShapeSettings settings(points.data(), static_cast<int>(points.size()));
                auto result = settings.Create();
                if (!result.IsValid())
                {
                    LH_CORE_WARN("ShapeCache: ConvexHullShape::Create failed for mesh '{}': {}",
                                 mesh.Name, std::string(result.GetError().c_str()));
                    return nullptr;
                }
                return result.Get();
            }

            if (kind == Type::MeshRef)
            {
                if (mesh.Indices.empty() || (mesh.Indices.size() % 3) != 0)
                {
                    LH_CORE_WARN("ShapeCache: MeshShape index count {} is not a positive multiple "
                                 "of 3 (mesh '{}')", mesh.Indices.size(), mesh.Name);
                    return nullptr;
                }

                JPH::VertexList verts;
                if (mesh.IsSkinned)
                {
                    verts.reserve(mesh.SkinnedVertices.size());
                    for (const auto& v : mesh.SkinnedVertices)
                        verts.emplace_back(v.Position.x, v.Position.y, v.Position.z);
                }
                else
                {
                    verts.reserve(mesh.Vertices.size());
                    for (const auto& v : mesh.Vertices)
                        verts.emplace_back(v.Position.x, v.Position.y, v.Position.z);
                }

                JPH::IndexedTriangleList tris;
                tris.reserve(mesh.Indices.size() / 3);
                for (size_t i = 0; i + 2 < mesh.Indices.size(); i += 3)
                {
                    tris.emplace_back(mesh.Indices[i], mesh.Indices[i + 1], mesh.Indices[i + 2], 0u);
                }

                JPH::MeshShapeSettings settings(std::move(verts), std::move(tris));
                auto result = settings.Create();
                if (!result.IsValid())
                {
                    LH_CORE_WARN("ShapeCache: MeshShape::Create failed for mesh '{}': {}",
                                 mesh.Name, std::string(result.GetError().c_str()));
                    return nullptr;
                }
                return result.Get();
            }

            return nullptr;
        }
    }

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
                break;  // fall through to asset path below
        }

        const UUID modelUUID(c.meshRef.modelHi, c.meshRef.modelLo);
        if (!modelUUID.IsValid())
            return { nullptr, false };

        const ShapeKey key{
            c.meshRef.modelHi,
            c.meshRef.modelLo,
            c.meshRef.meshIndex,
            static_cast<u8>(c.type),
            { 0, 0, 0 }
        };

        // Fast path — cache hit. Wrap with the per-collider offset and return.
        JPH::RefConst<JPH::Shape> inner;
        {
            SpinLockGuard guard(m_Lock);
            if (auto it = m_Map.find(key); it != m_Map.end())
                inner = it->second;
        }

        if (!inner)
        {
            // Cache miss. Resolve the model — if it hasn't loaded yet (in flight on a worker fiber)
            // the caller should retry next frame rather than dropping the body request.
            auto model = AssetManager::GetAsset<Model>(modelUUID);
            if (!model)
                return { nullptr, true };

            auto& meshes = model->GetMeshesData();
            if (c.meshRef.meshIndex >= meshes.size())
            {
                LH_CORE_WARN("ShapeCache: meshIndex {} out of range ({} meshes in model)",
                             c.meshRef.meshIndex, meshes.size());
                return { nullptr, false };
            }

            // invariant: Model::m_MeshesData persists post-upload — Model.cpp's ProcessMeshData
            // intentionally keeps CPU vertex/index data alive for queries. The future on-disk
            // physics-blob bake (`feat/jolt-cooked-shapes`) breaks this dependency.
            inner = BuildAssetShape(c.type, meshes[c.meshRef.meshIndex]);
            if (!inner)
                return { nullptr, false };

            // Insert under lock. If a parallel build raced us, the existing entry wins and ours is
            // dropped (Jolt's ref-count releases it).
            SpinLockGuard guard(m_Lock);
            auto [it, _] = m_Map.emplace(key, inner);
            inner = it->second;
        }

        return { WrapWithOffset(inner, c.localOffset, c.localRotation), false };
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

    u64 ShapeCache::ComputeFingerprint(const Component::Collider& c, const Component::RigidBody& rb)
    {
        u64 h = 1469598103934665603ull;
        auto mix64 = [&h](u64 v) {
            for (int i = 0; i < 8; ++i) { h ^= (v >> (i * 8)) & 0xff; h *= 1099511628211ull; }
        };
        auto mixf = [&mix64](f32 f) {
            u32 u = 0;
            std::memcpy(&u, &f, sizeof u);
            mix64(static_cast<u64>(u));
        };

        mix64(static_cast<u64>(c.type));
        mixf(c.localOffset.x); mixf(c.localOffset.y); mixf(c.localOffset.z);
        mixf(c.localRotation.x); mixf(c.localRotation.y);
        mixf(c.localRotation.z); mixf(c.localRotation.w);

        using Type = Component::Collider::Type;
        switch (c.type)
        {
            case Type::Box:
                mixf(c.boxHalfExtents.x); mixf(c.boxHalfExtents.y); mixf(c.boxHalfExtents.z);
                break;
            case Type::Sphere:
                mixf(c.sphereRadius);
                break;
            case Type::Capsule:
                mixf(c.capsule.radius); mixf(c.capsule.halfHeight);
                break;
            case Type::ConvexHullRef:
            case Type::MeshRef:
                mix64(c.meshRef.modelHi);
                mix64(c.meshRef.modelLo);
                mix64(static_cast<u64>(c.meshRef.meshIndex));
                break;
        }

        mix64(static_cast<u64>(rb.motion));
        mix64(static_cast<u64>(rb.layer));
        mix64(rb.isSensor ? 1ull : 0ull);
        mix64(static_cast<u64>(rb.motionQuality));
        mixf(rb.mass);
        return h;
    }
}
