#include "luthpch.h"

#include "luth/scene/systems/PhysicsSystem.h"

#include "luth/scene/Scene.h"
#include "luth/scene/components/Common.h"
#include "luth/scene/components/Transform.h"
#include "luth/scene/components/Physics.h"
#include "luth/physics/JoltMath.h"
#include "luth/physics/LayerMaskFilter.h"
#include "luth/physics/ShapeBuilder.h"
#include "luth/renderer/resources/Model.h"
#include "luth/resources/AssetDatabase.h"
#include "luth/resources/AssetManager.h"
#include "luth/core/DebugDraw.h"
#include "luth/core/EditorHooks.h"
#include "luth/core/diagnostics/Log.h"
#include "luth/core/diagnostics/Profiler.h"
#include "luth/core/time/Time.h"

#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Body/BodyInterface.h>
#include <Jolt/Physics/Body/BodyLock.h>
#include <Jolt/Physics/Body/BodyLockInterface.h>
#include <Jolt/Physics/Body/BodyManager.h>
#include <Jolt/Physics/Body/MotionQuality.h>
#include <Jolt/Physics/Body/MotionType.h>
#include <Jolt/Physics/Collision/CastResult.h>
#include <Jolt/Physics/Collision/CollideShape.h>
#include <Jolt/Physics/Collision/NarrowPhaseQuery.h>
#include <Jolt/Physics/Collision/RayCast.h>
#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <Jolt/Physics/Collision/Shape/CapsuleShape.h>
#include <Jolt/Physics/Collision/Shape/Shape.h>
#include <Jolt/Physics/Collision/Shape/SphereShape.h>
#include <Jolt/Physics/EActivation.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <span>
#include <unordered_set>

namespace Luth
{
    namespace
    {
        JPH::EMotionType ToJoltMotion(Component::RigidBody::Motion m)
        {
            switch (m)
            {
                case Component::RigidBody::Motion::Static:    return JPH::EMotionType::Static;
                case Component::RigidBody::Motion::Kinematic: return JPH::EMotionType::Kinematic;
                case Component::RigidBody::Motion::Dynamic:   return JPH::EMotionType::Dynamic;
            }
            return JPH::EMotionType::Static;
        }

        JPH::EMotionQuality ToJoltMotionQuality(Component::RigidBody::Quality q)
        {
            switch (q)
            {
                case Component::RigidBody::Quality::Discrete:   return JPH::EMotionQuality::Discrete;
                case Component::RigidBody::Quality::LinearCast: return JPH::EMotionQuality::LinearCast;
            }
            return JPH::EMotionQuality::Discrete;
        }

        JPH::ObjectLayer PickLayer(const Component::RigidBody& rb)
        {
            if (rb.motion == Component::RigidBody::Motion::Static) return Physics::Layers::STATIC;
            if (rb.isSensor)                                       return Physics::Layers::TRIGGER;
            return Physics::Layers::MOVING;
        }

        // ── Debug-draw helpers ──

        // Pack a float-RGBA in [0,1] into a u32 with the byte layout DebugDraw expects (byte 0 = R,
        // byte 3 = A) — same encoding as VK_FORMAT_R8G8B8A8_UNORM and JPH::Color::GetUInt32.
        u32 PackColor(const Vec4& c)
        {
            auto to8 = [](float x) { return static_cast<u32>(std::clamp(x, 0.0f, 1.0f) * 255.0f); };
            return to8(c.r) | (to8(c.g) << 8) | (to8(c.b) << 16) | (to8(c.a) << 24);
        }

        u32 SetAlpha(u32 color, float alpha)
        {
            const u32 a = static_cast<u32>(std::clamp(alpha, 0.0f, 1.0f) * 255.0f);
            return (color & 0x00ffffffu) | (a << 24);
        }

        u32 ScaleAlpha(u32 color, float scale)
        {
            const u32 a = (color >> 24) & 0xffu;
            const u32 a2 = static_cast<u32>(std::clamp(static_cast<float>(a) * scale, 0.0f, 255.0f));
            return (color & 0x00ffffffu) | (a2 << 24);
        }

        // Per-color-mode body colour. SleepColor mirrors Jolt's BodyManager::EShapeColor::SleepColor
        // (static grey, kinematic green, dynamic-active yellow, sleeping red); other modes use the
        // user's uniform colour or fixed motion-type swatches.
        u32 ResolveBaseColor(const Component::RigidBody& rb, const EditorViewportState& vs,
                             bool isActive)
        {
            using Motion = Component::RigidBody::Motion;
            switch (vs.physicsColorMode)
            {
                case PhysicsDebugColorMode::Uniform:
                    return PackColor(vs.physicsUniformColor);
                case PhysicsDebugColorMode::ByMotionType:
                    switch (rb.motion)
                    {
                        case Motion::Static:    return 0xff909090u;          // grey
                        case Motion::Kinematic: return 0xffdc8040u;          // blue
                        case Motion::Dynamic:   return 0xff5fdc5fu;          // green
                    }
                    break;
                case PhysicsDebugColorMode::BySleepState:
                    switch (rb.motion)
                    {
                        case Motion::Static:    return 0xff909090u;          // grey
                        case Motion::Kinematic: return 0xff5fdc5fu;          // green
                        case Motion::Dynamic:   return isActive ? 0xff5fdcdcu : 0xff5f5fdcu; // yellow / red
                    }
                    break;
            }
            return 0xffffffffu;
        }

        // Function-local cache of (cos, sin) pairs around the unit circle. Regenerated when the
        // segment count changes — single static is fine because PhysicsSystem::Update runs on the
        // game-stage fiber (single-writer).
        std::span<const Vec2> GetUnitCircle(u32 segments)
        {
            static std::vector<Vec2> table;
            static u32 cached = 0;
            if (segments != cached)
            {
                table.resize(segments);
                const float step = Math::TwoPi<f32> / static_cast<float>(segments);
                for (u32 i = 0; i < segments; ++i)
                    table[i] = Vec2(std::cos(static_cast<float>(i) * step),
                                    std::sin(static_cast<float>(i) * step));
                cached = segments;
            }
            return std::span<const Vec2>(table.data(), table.size());
        }

        // Emit a wire circle: lerp around the unit table, transform each point through `m` placing
        // the circle in the (axisU, axisV) plane scaled by radius, draw segment-to-segment lines.
        void DrawWireCircle(const Mat4& m, const Vec3& axisU, const Vec3& axisV, float radius,
                            u32 color, std::span<const Vec2> unit)
        {
            const Vec3 origin(m[3]);
            const Mat3 rot(m);
            const Vec3 u = rot * axisU * radius;
            const Vec3 v = rot * axisV * radius;
            const u32 n = static_cast<u32>(unit.size());
            for (u32 i = 0; i < n; ++i)
            {
                const Vec2& a = unit[i];
                const Vec2& b = unit[(i + 1) % n];
                const Vec3 pa = origin + u * a.x + v * a.y;
                const Vec3 pb = origin + u * b.x + v * b.y;
                Luth::DebugDraw::Line(pa, pb, color);
            }
        }

        // Wire sphere as three orthogonal great circles in the XY/XZ/YZ planes — Unity-style.
        void DrawWireSphere(const Mat4& m, float radius, u32 color, std::span<const Vec2> unit)
        {
            DrawWireCircle(m, Vec3(1, 0, 0), Vec3(0, 1, 0), radius, color, unit);
            DrawWireCircle(m, Vec3(1, 0, 0), Vec3(0, 0, 1), radius, color, unit);
            DrawWireCircle(m, Vec3(0, 1, 0), Vec3(0, 0, 1), radius, color, unit);
        }

        // Wire box: 12 edges from local (-h, +h) corners. The transform `m` places the box in the
        // world; rotation is preserved (oriented box, not axis-aligned).
        void DrawWireBox(const Mat4& m, const Vec3& halfExtents, u32 color)
        {
            Vec3 c[8];
            for (int i = 0; i < 8; ++i)
            {
                const Vec3 local((i & 1) ? halfExtents.x : -halfExtents.x,
                                 (i & 2) ? halfExtents.y : -halfExtents.y,
                                 (i & 4) ? halfExtents.z : -halfExtents.z);
                c[i] = Vec3(m * Vec4(local, 1.0f));
            }
            // Bottom (y = -h.y): 0,1,3,2
            Luth::DebugDraw::Line(c[0], c[1], color); Luth::DebugDraw::Line(c[1], c[3], color);
            Luth::DebugDraw::Line(c[3], c[2], color); Luth::DebugDraw::Line(c[2], c[0], color);
            // Top (y = +h.y): 4,5,7,6
            Luth::DebugDraw::Line(c[4], c[5], color); Luth::DebugDraw::Line(c[5], c[7], color);
            Luth::DebugDraw::Line(c[7], c[6], color); Luth::DebugDraw::Line(c[6], c[4], color);
            // Verticals
            Luth::DebugDraw::Line(c[0], c[4], color); Luth::DebugDraw::Line(c[1], c[5], color);
            Luth::DebugDraw::Line(c[2], c[6], color); Luth::DebugDraw::Line(c[3], c[7], color);
        }

        // Wire capsule along local Y axis (Jolt convention). Two equator circles at the cylinder
        // ends, two side seams along the cylinder, and one half-circle per hemisphere in each of
        // the XY/ZY planes — readable silhouette without going full icosphere.
        void DrawWireCapsule(const Mat4& m, float halfHeight, float radius, u32 color,
                             std::span<const Vec2> unit)
        {
            const Vec3 origin(m[3]);
            const Mat3 rot(m);
            const Vec3 axisX = rot * Vec3(1, 0, 0);
            const Vec3 axisY = rot * Vec3(0, 1, 0);
            const Vec3 axisZ = rot * Vec3(0, 0, 1);

            const Vec3 topCenter    = origin + axisY * halfHeight;
            const Vec3 bottomCenter = origin - axisY * halfHeight;

            // Equator circles at both cylinder caps (XZ plane in local).
            const u32 n = static_cast<u32>(unit.size());
            auto Circle = [&](const Vec3& centre, const Vec3& u, const Vec3& v) {
                for (u32 i = 0; i < n; ++i)
                {
                    const Vec2& a = unit[i];
                    const Vec2& b = unit[(i + 1) % n];
                    const Vec3 pa = centre + u * (a.x * radius) + v * (a.y * radius);
                    const Vec3 pb = centre + u * (b.x * radius) + v * (b.y * radius);
                    Luth::DebugDraw::Line(pa, pb, color);
                }
            };
            Circle(topCenter,    axisX, axisZ);
            Circle(bottomCenter, axisX, axisZ);

            // Side seams (4 vertical lines at ±X, ±Z on the cylinder).
            Luth::DebugDraw::Line(topCenter + axisX * radius, bottomCenter + axisX * radius, color);
            Luth::DebugDraw::Line(topCenter - axisX * radius, bottomCenter - axisX * radius, color);
            Luth::DebugDraw::Line(topCenter + axisZ * radius, bottomCenter + axisZ * radius, color);
            Luth::DebugDraw::Line(topCenter - axisZ * radius, bottomCenter - axisZ * radius, color);

            // Hemisphere arcs: half a great circle in each of the XY and ZY planes per cap.
            // Iterate the unit table but only emit segments whose endpoints lie on the correct
            // hemisphere (top: y >= 0; bottom: y <= 0). Arc plane uses (xAxis, yAxis) for one and
            // (zAxis, yAxis) for the other, capturing the four cardinal arc seams.
            auto HemiArc = [&](const Vec3& centre, const Vec3& planeU, const Vec3& planeV,
                               bool topHalf) {
                for (u32 i = 0; i < n; ++i)
                {
                    const Vec2& a = unit[i];
                    const Vec2& b = unit[(i + 1) % n];
                    const bool ay = topHalf ? (a.y >= 0.0f) : (a.y <= 0.0f);
                    const bool by = topHalf ? (b.y >= 0.0f) : (b.y <= 0.0f);
                    if (!ay || !by) continue;
                    const Vec3 pa = centre + planeU * (a.x * radius) + planeV * (a.y * radius);
                    const Vec3 pb = centre + planeU * (b.x * radius) + planeV * (b.y * radius);
                    Luth::DebugDraw::Line(pa, pb, color);
                }
            };
            HemiArc(topCenter,    axisX, axisY, /*topHalf*/true);
            HemiArc(topCenter,    axisZ, axisY, /*topHalf*/true);
            HemiArc(bottomCenter, axisX, axisY, /*topHalf*/false);
            HemiArc(bottomCenter, axisZ, axisY, /*topHalf*/false);
        }

        // Axis-aligned wire box from world-space corners — used for the AABB pass.
        void DrawWireAABB(const Vec3& mn, const Vec3& mx, u32 color)
        {
            const Vec3 c[8] = {
                { mn.x, mn.y, mn.z }, { mx.x, mn.y, mn.z },
                { mn.x, mx.y, mn.z }, { mx.x, mx.y, mn.z },
                { mn.x, mn.y, mx.z }, { mx.x, mn.y, mx.z },
                { mn.x, mx.y, mx.z }, { mx.x, mx.y, mx.z },
            };
            Luth::DebugDraw::Line(c[0], c[1], color); Luth::DebugDraw::Line(c[1], c[3], color);
            Luth::DebugDraw::Line(c[3], c[2], color); Luth::DebugDraw::Line(c[2], c[0], color);
            Luth::DebugDraw::Line(c[4], c[5], color); Luth::DebugDraw::Line(c[5], c[7], color);
            Luth::DebugDraw::Line(c[7], c[6], color); Luth::DebugDraw::Line(c[6], c[4], color);
            Luth::DebugDraw::Line(c[0], c[4], color); Luth::DebugDraw::Line(c[1], c[5], color);
            Luth::DebugDraw::Line(c[2], c[6], color); Luth::DebugDraw::Line(c[3], c[7], color);
        }

        // Local AABB of a primitive collider. Returns false for shapes we don't render yet
        // (ConvexHullRef / MeshRef) so the caller can skip them.
        bool ColliderLocalAABB(const Component::Collider& c, Vec3& outMin, Vec3& outMax)
        {
            using Type = Component::Collider::Type;
            switch (c.type)
            {
                case Type::Box:
                    outMin = -c.boxHalfExtents; outMax = c.boxHalfExtents; return true;
                case Type::Sphere:
                    outMin = Vec3(-c.sphereRadius); outMax = Vec3(c.sphereRadius); return true;
                case Type::Capsule: {
                    const float ext = c.capsule.halfHeight + c.capsule.radius;
                    outMin = Vec3(-c.capsule.radius, -ext, -c.capsule.radius);
                    outMax = Vec3( c.capsule.radius,  ext,  c.capsule.radius);
                    return true;
                }
                case Type::ConvexHullRef:
                case Type::MeshRef:
                    return false;
            }
            return false;
        }

        // Translate Jolt's body origin + identity orientation into a Mat4 for the CoM axis cross.
        // We don't need the body's full rotation for the cross — the axes are world-space — but
        // consumers may want the body's frame for future passes.
        Mat4 ComposeIdentity(const Vec3& origin)
        {
            Mat4 m(1.0f);
            m[3] = Vec4(origin, 1.0f);
            return m;
        }
    }

    PhysicsSystem::PhysicsSystem()
        : m_TempAlloc(kTempAllocatorBytes)
        , m_JobAdapter(kAdapterMaxJobs, kAdapterMaxBarriers)
        // entt::null is a templated null_t with a conversion operator to any entity-like type —
        // passed bare into vector's (size_type, const T&) it ambiguates with (size_type, Allocator&).
        // Materialise as entt::entity to pin the overload.
        , m_EntityByBodyIndex(kMaxBodies, entt::entity{entt::null})
        , m_ContactListener(m_Queue, std::span<entt::entity>(m_EntityByBodyIndex))
    {
        m_System.Init(kMaxBodies, kNumBodyMutexes, kMaxBodyPairs, kMaxContactConstraints,
                      m_BPLayers, m_OvBpFilter, m_LayerPairFilter);
        m_System.SetContactListener(&m_ContactListener);

        LH_CORE_INFO("PhysicsSystem initialized: {} max bodies, {} pairs, {} contacts",
                     kMaxBodies, kMaxBodyPairs, kMaxContactConstraints);
    }

    PhysicsSystem::~PhysicsSystem()
    {
        // Unhook the listener before draining bodies — any in-flight callback would access a
        // member that's about to be destroyed. The job system is already drained by the time we
        // get here (App teardown order), but the belt-and-suspenders unhook is cheap.
        m_System.SetContactListener(nullptr);
        DetachSignals();

        // Destroy any remaining bodies before m_System tears down. Skipping this leaks the body
        // manager's slots but is otherwise safe — Jolt's destructor cleans up the body table itself.
        // Doing it explicitly keeps allocator stats honest.
        if (!m_BodyMap.empty())
        {
            auto& bi = m_System.GetBodyInterface();
            for (auto& [_, id] : m_BodyMap)
            {
                bi.RemoveBody(id);
                bi.DestroyBody(id);
            }
            m_BodyMap.clear();
        }
        m_PendingDestroy.clear();
        m_PendingBuild.clear();
    }

    // ── Update ──

    void PhysicsSystem::Update(Scene* scene)
    {
        LH_PROFILE_FUNCTION();

        // Signal plumbing, deferred-destroy drain, and deferred-build drain all run regardless of
        // PlayState. Bailing under the Editing gate before signal connect leaves AddComponent calls
        // during scene load with no listener; deferring destroys/builds across frames means we'd
        // otherwise leak/skip them during authoring. Order matters: DrainPendingDestroys before
        // DrainPendingBuilds so a destroy + re-add of the same entity in one frame produces one
        // fresh body, not a destroyed dangling one.
        EnsureSignalsConnected(scene);
        EnsureChangeCallbackRegistered();
        DrainDirtyAssets(scene);
        DrainPendingDestroys();
        DrainPendingBuilds(scene);

        // Debug-draw runs unconditionally so colliders are visible while authoring (Editing) too.
        // Reads body state from JPH::PhysicsSystem; safe between Step calls.
        DrawDebugBodies(scene);

        // Editor gate. When no editor is registered (runtime-only build) Get() is null and the sim ticks
        // unconditionally. Paused + ConsumeStepRequest() to advance one step lands alongside body
        // lifecycle, when there is something visible to step.
        if (auto* hooks = EditorHooks::Get();
            hooks && hooks->GetPlayState() == PlayState::Editing)
        {
            m_Accumulator = 0.0f;
            return;
        }

        m_Accumulator += Time::DeltaTime();

        // Cap the accumulator so a frame stall doesn't spiral. Lost time is acceptable here — the alt is
        // an unbounded catch-up loop that makes a stall worse.
        const f32 maxAccum = kMaxSubSteps * kFixedDt;
        if (m_Accumulator > maxAccum)
            m_Accumulator = maxAccum;

        const int substeps = static_cast<int>(m_Accumulator / kFixedDt);
        if (substeps <= 0)
            return;

        // Kinematic bodies move once per frame at the user-set pose. Pre-step sync writes the target;
        // Jolt interpolates velocity over the substep block so collisions resolve as if the body moved
        // smoothly across the whole frame.
        SyncTransformsToBodies(scene);

        for (int i = 0; i < substeps; ++i)
        {
            Step(kFixedDt, /*collisionSteps*/1);
            m_Accumulator -= kFixedDt;
        }

        // Dynamic bodies write their post-step pose back to Transform / WorldTransform. Marking the
        // Transform dirty queues TransformSystem to rebuild LocalMatrix next frame.
        SyncBodiesToTransforms(scene);
    }

    void PhysicsSystem::Step(f32 dt, int collisionSteps)
    {
        LH_PROFILE_FUNCTION();

        // Query re-entry guard. Set across the entire Jolt update — including the listener-callback
        // window — so any nested Raycast/Overlap from gameplay code reachable through a callback
        // trips the assert instead of deadlocking on body locks.
        m_StepInFlight.store(true, std::memory_order_release);
        m_System.Update(dt, collisionSteps, &m_TempAlloc, &m_JobAdapter);
        m_StepInFlight.store(false, std::memory_order_release);
    }

    void PhysicsSystem::DrawDebugBodies(Scene* scene)
    {
        LH_PROFILE_FUNCTION();
        if (!scene) return;

        // Skip entirely in runtime builds — there's no editor to drive the toggles, and the only
        // sane runtime default is "off". A headless smoke harness can still emit debug lines via
        // DebugDraw directly if needed.
        auto* hooks = EditorHooks::Get();
        if (!hooks) return;

        EditorViewportState vs{};
        hooks->GetViewportState(vs);

        const bool wantShapes = vs.physicsShapesSelected || vs.physicsShapesAll;
        const bool wantAABBs  = vs.physicsAABBsSelected  || vs.physicsAABBsAll;
        const bool wantCoM    = vs.physicsCoMSelected    || vs.physicsCoMAll;
        if (!wantShapes && !wantAABBs && !wantCoM) return;

        std::unordered_set<entt::entity> selected;
        selected.reserve(vs.selectedEntities.size());
        for (const Entity& e : vs.selectedEntities)
            selected.insert(static_cast<entt::entity>(e));

        const u32 segments = std::clamp<u32>(vs.physicsDebugSegments, 8u, 128u);
        const std::span<const Vec2> unit = GetUnitCircle(segments);

        auto& reg = scene->Registry();
        auto& bi  = m_System.GetBodyInterface();

        // Single iteration over the (Collider + WorldTransform + RigidBody + PhysicsBodyRuntime)
        // view feeds all three passes — cheaper than walking three times for typical body counts.
        auto view = reg.view<Component::Collider, Component::WorldTransform,
                             Component::RigidBody, Component::PhysicsBodyRuntime>();
        for (auto entity : view)
        {
            const auto& collider = view.get<Component::Collider>(entity);
            const auto& world    = view.get<Component::WorldTransform>(entity);
            const auto& rb       = view.get<Component::RigidBody>(entity);
            const auto& runtime  = view.get<Component::PhysicsBodyRuntime>(entity);

            const bool isSelected = selected.contains(entity);

            const bool drawShape = wantShapes && (vs.physicsShapesAll || (isSelected && vs.physicsShapesSelected));
            const bool drawAABB  = wantAABBs  && (vs.physicsAABBsAll  || (isSelected && vs.physicsAABBsSelected));
            const bool drawCoM   = wantCoM    && (vs.physicsCoMAll    || (isSelected && vs.physicsCoMSelected));
            if (!drawShape && !drawAABB && !drawCoM) continue;

            // BySleepState is the only mode that needs an IsActive query; skip for the others.
            const bool needsActive = vs.physicsColorMode == PhysicsDebugColorMode::BySleepState;
            const bool isActive = (needsActive && runtime.bodyId != Component::PhysicsBodyRuntime::cInvalidBodyId)
                                ? bi.IsActive(JPH::BodyID(runtime.bodyId)) : true;

            u32 color = ResolveBaseColor(rb, vs, isActive);
            if (!isSelected) color = SetAlpha(color, vs.physicsAlphaUnselected);
            if (rb.isSensor) color = ScaleAlpha(color, 0.5f);

            // Compose the collider-local frame: world * translate(localOffset) * rotate(localRot).
            const Mat4 localT  = glm::translate(Mat4(1.0f), collider.localOffset);
            const Mat4 localR  = glm::mat4_cast(collider.localRotation);
            const Mat4 colMat  = world.Matrix * localT * localR;

            if (drawShape)
            {
                using Type = Component::Collider::Type;
                switch (collider.type)
                {
                    case Type::Box:
                        DrawWireBox(colMat, collider.boxHalfExtents, color);
                        break;
                    case Type::Sphere:
                        DrawWireSphere(colMat, collider.sphereRadius, color, unit);
                        break;
                    case Type::Capsule:
                        DrawWireCapsule(colMat, collider.capsule.halfHeight,
                                        collider.capsule.radius, color, unit);
                        break;
                    case Type::ConvexHullRef:
                    case Type::MeshRef: {
                        // Shape baking lands later (Phase E). Render the local AABB as a placeholder
                        // so the entity is still visible — keeps the debug view honest about "this
                        // body has no real shape yet".
                        Vec3 mn, mx;
                        if (ColliderLocalAABB(collider, mn, mx))
                            DrawWireBox(colMat, (mx - mn) * 0.5f, color);
                        break;
                    }
                }
            }

            if (drawAABB)
            {
                Vec3 lmn, lmx;
                if (ColliderLocalAABB(collider, lmn, lmx))
                {
                    // 8-corner transform → world-space min/max. Slight overestimation under rotation
                    // for sphere/capsule; acceptable for a debug visualization.
                    Vec3 wmn(std::numeric_limits<float>::max());
                    Vec3 wmx(std::numeric_limits<float>::lowest());
                    for (int i = 0; i < 8; ++i)
                    {
                        const Vec3 local((i & 1) ? lmx.x : lmn.x,
                                         (i & 2) ? lmx.y : lmn.y,
                                         (i & 4) ? lmx.z : lmn.z);
                        const Vec3 wp = Vec3(colMat * Vec4(local, 1.0f));
                        wmn = glm::min(wmn, wp);
                        wmx = glm::max(wmx, wp);
                    }
                    DrawWireAABB(wmn, wmx, color);
                }
            }

            if (drawCoM && runtime.bodyId != Component::PhysicsBodyRuntime::cInvalidBodyId)
            {
                // Jolt's CoM is the volumetric centre of the shape; for off-centre colliders this
                // diverges from the entity origin. Querying the body keeps it accurate.
                const Vec3 com = Physics::FromJolt(bi.GetCenterOfMassPosition(JPH::BodyID(runtime.bodyId)));
                const Mat4 comMat = ComposeIdentity(com);
                const float axisLen = 0.25f;
                const u32 axisAlpha = (color >> 24) & 0xffu;
                const u32 redA   = 0x000000ffu | (axisAlpha << 24);
                const u32 greenA = 0x0000ff00u | (axisAlpha << 24);
                const u32 blueA  = 0x00ff0000u | (axisAlpha << 24);
                const Vec3 origin(comMat[3]);
                Luth::DebugDraw::Line(origin, origin + Vec3(axisLen, 0, 0), redA);
                Luth::DebugDraw::Line(origin, origin + Vec3(0, axisLen, 0), greenA);
                Luth::DebugDraw::Line(origin, origin + Vec3(0, 0, axisLen), blueA);
            }
        }
    }

    // ── Signals ──

    void PhysicsSystem::EnsureSignalsConnected(Scene* scene)
    {
        if (!scene) return;
        auto* reg = &scene->Registry();
        if (reg == m_AttachedRegistry) return;

        // Scene change. Tear down old bodies and signals, then attach fresh.
        if (m_AttachedRegistry)
        {
            DetachSignals();
            if (!m_BodyMap.empty())
            {
                auto& bi = m_System.GetBodyInterface();
                for (auto& [_, id] : m_BodyMap)
                {
                    bi.RemoveBody(id);
                    bi.DestroyBody(id);
                }
                m_BodyMap.clear();
            }
            m_PendingDestroy.clear();
            m_PendingBuild.clear();
            // Drop cached asset shapes — model UUIDs may collide across projects after
            // UnloadProject + LoadProject. Worst case is a fresh build per shape on first body
            // create in the new scene; serving stale geometry to a different project would be
            // a correctness bug.
            m_ShapeCache.Clear();
        }

        m_AttachedRegistry = reg;
        m_AttachedScene = scene;
        reg->on_construct<Component::Collider>().connect<&PhysicsSystem::OnComponentConstructed>(this);
        reg->on_construct<Component::RigidBody>().connect<&PhysicsSystem::OnComponentConstructed>(this);
        reg->on_update<Component::Collider>().connect<&PhysicsSystem::OnComponentUpdated>(this);
        reg->on_update<Component::RigidBody>().connect<&PhysicsSystem::OnComponentUpdated>(this);
        reg->on_destroy<Component::Collider>().connect<&PhysicsSystem::OnComponentDestroyed>(this);
        reg->on_destroy<Component::RigidBody>().connect<&PhysicsSystem::OnComponentDestroyed>(this);
    }

    void PhysicsSystem::DetachSignals()
    {
        if (!m_AttachedRegistry) return;
        auto* reg = m_AttachedRegistry;
        reg->on_construct<Component::Collider>().disconnect<&PhysicsSystem::OnComponentConstructed>(this);
        reg->on_construct<Component::RigidBody>().disconnect<&PhysicsSystem::OnComponentConstructed>(this);
        reg->on_update<Component::Collider>().disconnect<&PhysicsSystem::OnComponentUpdated>(this);
        reg->on_update<Component::RigidBody>().disconnect<&PhysicsSystem::OnComponentUpdated>(this);
        reg->on_destroy<Component::Collider>().disconnect<&PhysicsSystem::OnComponentDestroyed>(this);
        reg->on_destroy<Component::RigidBody>().disconnect<&PhysicsSystem::OnComponentDestroyed>(this);
        m_AttachedRegistry = nullptr;
    }

    void PhysicsSystem::OnComponentConstructed(entt::registry& /*reg*/, entt::entity entity)
    {
        // Defer the build to Update start. Inline TryCreateBody here would race with the Inspector's
        // "Add Component → user edits fields" pattern: on_construct fires synchronously inside
        // AddComponent before the user can write any field, so the body would be built with default
        // values and the later edits would no-op against an already-built body. Queueing means the
        // build picks up whatever the component looks like at next Update — and on_update edits
        // also queue, so a body always reflects the latest fields after one frame.
        QueueBuild(entity);
    }

    void PhysicsSystem::OnComponentUpdated(entt::registry& /*reg*/, entt::entity entity)
    {
        // patch<>() on either Collider or RigidBody routes here. Queue a rebuild — DrainPendingBuilds
        // tears down the existing body before reissuing TryCreateBody so shape, mass, layer, motion
        // type, and motion quality are all picked up from the latest component state.
        QueueBuild(entity);
    }

    void PhysicsSystem::OnComponentDestroyed(entt::registry& reg, entt::entity entity)
    {
        // Either component leaving invalidates the pair; queue the body for deferred destruction so we
        // don't mutate Jolt state from inside an entity-destroy signal.
        DestroyBodyForEntity(reg, entity);
    }

    // ── Body lifecycle ──

    PhysicsSystem::BuildResult PhysicsSystem::TryCreateBody(Scene* scene, entt::entity entity)
    {
        auto& reg = scene->Registry();
        if (m_BodyMap.contains(entity)) return BuildResult::Failed;
        if (!reg.all_of<Component::Collider, Component::RigidBody, Component::Transform>(entity))
            return BuildResult::Failed;

        const auto& collider  = reg.get<Component::Collider>(entity);
        const auto& rb        = reg.get<Component::RigidBody>(entity);
        const auto& transform = reg.get<Component::Transform>(entity);

        // Mesh / heightfield shapes are static-only in Jolt. Refuse the pairing rather than crash later.
        if ((collider.type == Component::Collider::Type::MeshRef) &&
            rb.motion != Component::RigidBody::Motion::Static)
        {
            LH_CORE_WARN("PhysicsSystem: MeshRef requires Static body — skipping entity {}",
                         (u32)entity);
            return BuildResult::Failed;
        }

        auto outcome = m_ShapeCache.GetOrBuild(collider);
        if (!outcome.shape)
        {
            // RetryLater is set by the cache when the source asset hasn't loaded yet. Permanent
            // failures (invalid primitive params, opt-out, missing UUID) drop with retryLater=false
            // so the caller stops cycling them through the queue.
            return outcome.retryLater ? BuildResult::RetryLater : BuildResult::Failed;
        }
        auto shape = outcome.shape;

        // Pin the source Model so AssetManager::Trim can't evict it while a body still references
        // its vertex data. Mirrors the RenderSnapshot/MeshRenderer pattern; HoldAsset is idempotent
        // on the same UUID. Materials follow in the PhysicsMaterial sub-task.
        if (collider.type == Component::Collider::Type::ConvexHullRef
            || collider.type == Component::Collider::Type::MeshRef)
        {
            const UUID modelUUID(collider.meshRef.modelHi, collider.meshRef.modelLo);
            if (auto model = AssetManager::GetAsset<Model>(modelUUID))
                scene->HoldAsset(modelUUID, model);
        }

        // Source the initial body pose from Transform (local) rather than WorldTransform: signals fire
        // during scene-load AddComponent calls, before TransformSystem has had a chance to propagate
        // local-to-world. Tier 0 contract is that physics-driven entities sit at the scene root, so
        // local == world here. Parented entities are warned below; their initial pose will be wrong
        // until TransformSystem catches up next frame.
        const Vec3 pos = transform.Position;
        const Quat rot = Quat(Math::Radians(transform.Rotation));

        if (reg.any_of<Component::Parent>(entity))
        {
            LH_CORE_WARN("PhysicsSystem: entity {} has a Parent — physics-driven Transform sync ignores"
                         " hierarchy and may drift", (u32)entity);
        }

        JPH::BodyCreationSettings bcs(shape, Physics::ToJolt(pos), Physics::ToJolt(rot),
                                      ToJoltMotion(rb.motion), PickLayer(rb));
        bcs.mIsSensor        = rb.isSensor;
        bcs.mMotionQuality   = ToJoltMotionQuality(rb.motionQuality);
        bcs.mLinearVelocity  = Physics::ToJolt(rb.linearVelocity);
        bcs.mAngularVelocity = Physics::ToJolt(rb.angularVelocity);
        bcs.mGravityFactor   = rb.gravityFactor;
        bcs.mLinearDamping   = rb.linearDamping;
        bcs.mAngularDamping  = rb.angularDamping;

        if (rb.mass > 0.0f)
        {
            bcs.mOverrideMassProperties        = JPH::EOverrideMassProperties::CalculateInertia;
            bcs.mMassPropertiesOverride.mMass  = rb.mass;
        }

        const auto activation = (rb.startActive && rb.motion != Component::RigidBody::Motion::Static)
                              ? JPH::EActivation::Activate : JPH::EActivation::DontActivate;

        auto& bi = m_System.GetBodyInterface();
        const JPH::BodyID id = bi.CreateAndAddBody(bcs, activation);
        if (id.IsInvalid())
        {
            LH_CORE_WARN("PhysicsSystem: CreateAndAddBody failed for entity {}", (u32)entity);
            return BuildResult::Failed;
        }

        // Pack entity into user data for reverse lookup from contact callbacks. OnContactAdded /
        // OnContactPersisted read it via Body::GetUserData; OnContactRemoved can't access bodies
        // (ContactListener.h:127), so we also fill m_EntityByBodyIndex below for that path.
        bi.SetUserData(id, static_cast<JPH::uint64>(entity));

        // Side table for OnContactRemoved. Index is BodyID's 23-bit slot index — bounded by
        // kMaxBodies, so an array indexed by GetIndex() is O(1) and bounded ~64 KB.
        const u32 bodyIdx = id.GetIndex();
        if (bodyIdx < m_EntityByBodyIndex.size())
            m_EntityByBodyIndex[bodyIdx] = entity;

        m_BodyMap[entity] = id;
        auto& runtime = reg.get_or_emplace<Component::PhysicsBodyRuntime>(entity);
        runtime.bodyId = id.GetIndexAndSequenceNumber();
        runtime.shapeFingerprint = 0;
        return BuildResult::Created;
    }

    void PhysicsSystem::DestroyBodyForEntity(entt::registry& /*reg*/, entt::entity entity)
    {
        auto it = m_BodyMap.find(entity);
        if (it == m_BodyMap.end()) return;

        // Queue + remove from m_BodyMap immediately so a paired on_destroy (e.g. RigidBody after
        // Collider) sees an empty entry and short-circuits.
        m_PendingDestroy.push_back({entity, it->second});
        m_BodyMap.erase(it);
    }

    void PhysicsSystem::DrainPendingDestroys()
    {
        if (m_PendingDestroy.empty()) return;

        auto& bi = m_System.GetBodyInterface();
        for (const auto& pd : m_PendingDestroy)
        {
            bi.RemoveBody(pd.bodyId);
            bi.DestroyBody(pd.bodyId);

            // Clear the side-table slot. A subsequent body creation may land on the same index
            // with a new sequence number; the entry just gets re-populated at TryCreateBody.
            const u32 bodyIdx = pd.bodyId.GetIndex();
            if (bodyIdx < m_EntityByBodyIndex.size())
                m_EntityByBodyIndex[bodyIdx] = entt::null;

            if (m_AttachedRegistry && m_AttachedRegistry->valid(pd.entity)
                && m_AttachedRegistry->any_of<Component::PhysicsBodyRuntime>(pd.entity))
            {
                m_AttachedRegistry->remove<Component::PhysicsBodyRuntime>(pd.entity);
            }
        }
        m_PendingDestroy.clear();
    }

    void PhysicsSystem::QueueBuild(entt::entity entity)
    {
        // Cheap append. Dedup runs once at drain time — sort+unique scales better than per-insert
        // std::find when many entities are queued in one frame (e.g. scene load).
        m_PendingBuild.push_back(entity);
    }

    void PhysicsSystem::DrainPendingBuilds(Scene* scene)
    {
        if (m_PendingBuild.empty() || !scene) return;

        std::sort(m_PendingBuild.begin(), m_PendingBuild.end());
        m_PendingBuild.erase(std::unique(m_PendingBuild.begin(), m_PendingBuild.end()),
                             m_PendingBuild.end());

        auto& bi = m_System.GetBodyInterface();

        // Shadow vector for retries — entities whose source asset hasn't loaded yet stay queued
        // for next Update. Bounded by the count of asset-backed bodies in the scene; per-frame
        // work is ~one BodyMap lookup per pending entity until the asset arrives.
        std::vector<entt::entity> retry;

        for (auto entity : m_PendingBuild)
        {
            // Tear down the existing body before rebuild. Safe to do synchronously here — drain runs
            // at Update start, before any Step, so no in-flight simulation depends on the body.
            if (auto it = m_BodyMap.find(entity); it != m_BodyMap.end())
            {
                bi.RemoveBody(it->second);
                bi.DestroyBody(it->second);
                m_BodyMap.erase(it);
            }

            // TryCreateBody re-emplaces PhysicsBodyRuntime with the new bodyId on success, returns
            // RetryLater if the source asset isn't loaded yet (re-queue), and Failed for everything
            // else — bad components, opt-out, invalid params (drop).
            if (TryCreateBody(scene, entity) == BuildResult::RetryLater)
                retry.push_back(entity);
        }
        m_PendingBuild.swap(retry);
    }

    void PhysicsSystem::DrainDirtyAssets(Scene* scene)
    {
        if (!scene) return;

        // Snapshot under lock so the callback can keep pushing while we walk the registry.
        std::vector<UUID> dirty;
        {
            SpinLockGuard guard(m_DirtyAssetsLock);
            if (m_DirtyAssetsScratch.empty()) return;
            dirty.swap(m_DirtyAssetsScratch);
        }

        m_ShapeCache.OnAssetReimported(dirty);

        // Rebuild bodies whose Collider references one of the dirty UUIDs. Linear scan over the
        // dirty list per collider is fine — dirty lists from FileWatcher are typically 1-3 UUIDs.
        auto& reg = scene->Registry();
        auto view = reg.view<Component::Collider>();
        for (auto entity : view)
        {
            const auto& c = view.get<Component::Collider>(entity);
            if (c.type != Component::Collider::Type::ConvexHullRef
                && c.type != Component::Collider::Type::MeshRef) continue;
            const UUID model(c.meshRef.modelHi, c.meshRef.modelLo);
            for (const UUID& d : dirty)
            {
                if (d == model) { QueueBuild(entity); break; }
            }
        }
    }

    void PhysicsSystem::EnsureChangeCallbackRegistered()
    {
        if (m_ChangeCallbackRegistered) return;

        // Bound to `this` for the lifetime of the App. AssetDatabase has no unregister API today;
        // PhysicsSystem outlives the App loop so the dangling-callback hazard is theoretical, not
        // observable. The callback only stages UUIDs — registry walks happen on the game-stage
        // fiber via DrainDirtyAssets.
        AssetDatabase::AddChangeCallback([this]() {
            const auto& dirty = AssetDatabase::GetDirtyAssets();
            if (dirty.empty()) return;
            SpinLockGuard guard(m_DirtyAssetsLock);
            m_DirtyAssetsScratch.insert(m_DirtyAssetsScratch.end(), dirty.begin(), dirty.end());
        });
        m_ChangeCallbackRegistered = true;
    }

    // ── Transform <-> Body sync ──

    void PhysicsSystem::SyncTransformsToBodies(Scene* scene)
    {
        LH_PROFILE_FUNCTION();
        if (!scene) return;

        auto& reg = scene->Registry();
        auto& bi  = m_System.GetBodyInterface();
        const f32 dt = kFixedDt;

        auto view = reg.view<Component::PhysicsBodyRuntime, Component::RigidBody, Component::WorldTransform>();
        for (auto entity : view)
        {
            const auto& rb = view.get<Component::RigidBody>(entity);
            if (rb.motion != Component::RigidBody::Motion::Kinematic) continue;

            const auto& runtime = view.get<Component::PhysicsBodyRuntime>(entity);
            if (runtime.bodyId == Component::PhysicsBodyRuntime::cInvalidBodyId) continue;

            const auto& world = view.get<Component::WorldTransform>(entity);
            Vec3 pos, scale;
            Quat rot;
            DecomposeTransform(world.Matrix, pos, rot, scale);

            bi.MoveKinematic(JPH::BodyID(runtime.bodyId),
                             Physics::ToJolt(pos), Physics::ToJolt(rot), dt);
        }
    }

    void PhysicsSystem::SyncBodiesToTransforms(Scene* scene)
    {
        LH_PROFILE_FUNCTION();
        if (!scene) return;

        auto& reg = scene->Registry();
        auto& bi  = m_System.GetBodyInterface();

        auto view = reg.view<Component::PhysicsBodyRuntime, Component::RigidBody,
                              Component::Transform, Component::WorldTransform>();
        for (auto entity : view)
        {
            const auto& rb = view.get<Component::RigidBody>(entity);
            if (rb.motion != Component::RigidBody::Motion::Dynamic) continue;

            const auto& runtime = view.get<Component::PhysicsBodyRuntime>(entity);
            if (runtime.bodyId == Component::PhysicsBodyRuntime::cInvalidBodyId) continue;

            const JPH::BodyID id(runtime.bodyId);
            const JPH::Vec3 pos = bi.GetPosition(id);
            const JPH::Quat rot = bi.GetRotation(id);
            const JPH::Vec3 lvel = bi.GetLinearVelocity(id);
            const JPH::Vec3 avel = bi.GetAngularVelocity(id);

            // Write back to local Transform — correct for root entities and the documented Tier 0
            // contract; parented entities drift (warned at body-create time). WorldTransform is also
            // updated directly so render systems reading it this frame see the post-step pose.
            auto& transform = view.get<Component::Transform>(entity);
            auto& world     = view.get<Component::WorldTransform>(entity);
            auto& rbMut     = const_cast<Component::RigidBody&>(rb);

            const Quat localRot = Physics::FromJolt(rot);
            transform.Position = Physics::FromJolt(pos);
            transform.Rotation = glm::eulerAngles(localRot) * Math::RadToDeg<f32>;
            transform.IsDirty  = true;

            world.Matrix = ComposeTransform(transform.Position, localRot, transform.Scale);

            rbMut.linearVelocity  = Physics::FromJolt(lvel);
            rbMut.angularVelocity = Physics::FromJolt(avel);
        }
    }

    // ── Queries ──

    bool PhysicsSystem::Raycast(const Vec3& origin, const Vec3& dir, f32 maxDist,
                                u32 layerMask, Physics::RaycastHit& outHit) const
    {
        LH_PROFILE_FUNCTION();
        LH_CORE_ASSERT(!m_StepInFlight.load(std::memory_order_acquire),
                       "PhysicsSystem::Raycast called during Step — NarrowPhaseQuery is not safe "
                       "to call from a contact callback or any code reachable from m_System.Update");

        // Jolt's CastRay takes (origin, direction*length) — anything past mDirection's length is
        // not reported. Encoding maxDist into the direction keeps the public API split (caller
        // passes a unit-ish direction + a distance) without forcing them to normalise.
        const JPH::RRayCast ray(Physics::ToJolt(origin), Physics::ToJolt(dir * maxDist));

        Physics::LayerMaskBroadPhaseFilter bpFilter(layerMask);
        Physics::LayerMaskObjectFilter     objFilter(layerMask);

        JPH::RayCastResult hit;
        auto& npq = m_System.GetNarrowPhaseQuery();
        if (!npq.CastRay(ray, hit, bpFilter, objFilter))
            return false;

        // Resolve normal + entity. The body lock interface is the read-only path — safe outside
        // Step. SucceededAndIsInBroadPhase confirms the body still exists between hit detection
        // and our follow-up read (a body destroyed in between is a non-issue at Tier-0 since
        // queries are called from gameplay code on the main fiber, not from contact callbacks).
        const auto& bli = m_System.GetBodyLockInterface();
        JPH::BodyLockRead lock(bli, hit.mBodyID);
        if (!lock.SucceededAndIsInBroadPhase())
            return false;

        const JPH::Body& body = lock.GetBody();
        const JPH::Vec3  worldPoint = ray.GetPointOnRay(hit.mFraction);
        const JPH::Vec3  worldNormal = body.GetWorldSpaceSurfaceNormal(hit.mSubShapeID2, worldPoint);
        const auto       entity = static_cast<entt::entity>(body.GetUserData());

        outHit.entity      = entity;
        outHit.bodyId      = hit.mBodyID;
        outHit.worldPoint  = Physics::FromJolt(worldPoint);
        outHit.worldNormal = Physics::FromJolt(worldNormal);
        outHit.fraction    = hit.mFraction;
        outHit.distance    = hit.mFraction * maxDist;
        return true;
    }

    namespace
    {
        // CollideShape collector that funnels hits into a caller-provided span and clamps when full.
        // ForceEarlyOut terminates the broadphase walk; further AddHit calls would be unbounded work
        // we'd just throw away. Body lookup uses the no-lock body interface — query is single-threaded
        // and outside Step, so the broadphase isn't mutating concurrently.
        class SpanOverlapCollector final : public JPH::CollideShapeCollector
        {
        public:
            SpanOverlapCollector(std::span<Physics::OverlapHit> out, const JPH::BodyLockInterface& bli)
                : m_Out(out), m_Bli(bli) {}

            void AddHit(const JPH::CollideShapeResult& result) override
            {
                if (m_Count >= m_Out.size())
                {
                    ForceEarlyOut();
                    return;
                }

                entt::entity entity = entt::null;
                JPH::BodyLockRead lock(m_Bli, result.mBodyID2);
                if (lock.SucceededAndIsInBroadPhase())
                    entity = static_cast<entt::entity>(lock.GetBody().GetUserData());

                m_Out[m_Count++] = { entity, result.mBodyID2 };
                if (m_Count >= m_Out.size())
                    ForceEarlyOut();
            }

            u32 Count() const { return m_Count; }

        private:
            std::span<Physics::OverlapHit> m_Out;
            const JPH::BodyLockInterface&  m_Bli;
            u32                             m_Count = 0;
        };
    }

    u32 PhysicsSystem::OverlapShape(const JPH::Shape* shape, const Vec3& center, const Quat& rot,
                                    u32 layerMask, std::span<Physics::OverlapHit> outHits) const
    {
        LH_PROFILE_FUNCTION();
        LH_CORE_ASSERT(!m_StepInFlight.load(std::memory_order_acquire),
                       "PhysicsSystem::Overlap* called during Step");
        if (!shape || outHits.empty()) return 0;

        // baseOffset == center: returns hit points relative to center for better float precision
        // when the query lives far from the world origin. Doesn't affect entity/BodyID resolution.
        const JPH::Vec3   joltCenter = Physics::ToJolt(center);
        const JPH::Quat   joltRot    = Physics::ToJolt(rot);
        const JPH::RMat44 com        = JPH::RMat44::sRotationTranslation(joltRot, joltCenter);

        JPH::CollideShapeSettings settings;
        Physics::LayerMaskBroadPhaseFilter bpFilter(layerMask);
        Physics::LayerMaskObjectFilter     objFilter(layerMask);

        SpanOverlapCollector collector(outHits, m_System.GetBodyLockInterface());
        m_System.GetNarrowPhaseQuery().CollideShape(shape, JPH::Vec3::sOne(), com, settings,
                                                    joltCenter, collector, bpFilter, objFilter);
        return collector.Count();
    }

    u32 PhysicsSystem::OverlapBox(const Vec3& center, const Vec3& halfExtents, const Quat& rot,
                                  u32 layerMask, std::span<Physics::OverlapHit> outHits) const
    {
        if (halfExtents.x <= 0.0f || halfExtents.y <= 0.0f || halfExtents.z <= 0.0f) return 0;
        JPH::BoxShapeSettings settings(Physics::ToJolt(halfExtents));
        auto result = settings.Create();
        if (!result.IsValid()) return 0;
        return OverlapShape(result.Get().GetPtr(), center, rot, layerMask, outHits);
    }

    u32 PhysicsSystem::OverlapSphere(const Vec3& center, f32 radius,
                                     u32 layerMask, std::span<Physics::OverlapHit> outHits) const
    {
        if (radius <= 0.0f) return 0;
        JPH::SphereShapeSettings settings(radius);
        auto result = settings.Create();
        if (!result.IsValid()) return 0;
        // Sphere rotation has no geometric effect; pass identity through OverlapShape.
        return OverlapShape(result.Get().GetPtr(), center, Quat(1.0f, 0.0f, 0.0f, 0.0f),
                            layerMask, outHits);
    }

    u32 PhysicsSystem::OverlapCapsule(const Vec3& center, f32 radius, f32 halfHeight,
                                      const Quat& rot, u32 layerMask,
                                      std::span<Physics::OverlapHit> outHits) const
    {
        if (radius <= 0.0f || halfHeight <= 0.0f) return 0;
        JPH::CapsuleShapeSettings settings(halfHeight, radius);
        auto result = settings.Create();
        if (!result.IsValid()) return 0;
        return OverlapShape(result.Get().GetPtr(), center, rot, layerMask, outHits);
    }

    // ── Event drain ──

    u32 PhysicsSystem::DrainEvents(std::span<Physics::PhysicsEvent> outEvents)
    {
        LH_PROFILE_FUNCTION();
        if (outEvents.empty()) return 0;

        u32 written = 0;
        Physics::PhysicsEvent ev;
        while (written < outEvents.size() && m_Queue.TryPop(ev))
            outEvents[written++] = ev;

        // Overflow warning is once-per-frame: the queue rejected N events because gameplay didn't
        // drain fast enough or the scene's contact rate exceeded MPMCQueue capacity. Bumping 4096
        // is cheap; reaching it consistently means gameplay needs a per-frame DrainEvents call
        // sized to the scene.
        if (const u32 lost = m_ContactListener.ConsumeOverflowCount(); lost > 0)
            LH_CORE_WARN("PhysicsSystem: dropped {} contact event(s) - queue saturated", lost);

        return written;
    }
}
