#pragma once

#include <Jolt/Jolt.h>

#include <Jolt/Physics/Collision/BroadPhase/BroadPhaseLayer.h>
#include <Jolt/Physics/Collision/ObjectLayer.h>

namespace Luth::Physics
{
    // Object layers describe the role of a body for filtering. STATIC = scenery and kinematic ground;
    // MOVING = dynamic rigid bodies and the character controller; TRIGGER = static sensor zones that fire
    // contact events when MOVING bodies enter or exit them. Dynamic sensors stay on MOVING with the
    // RigidBody.isSensor flag — the layer split here is purely for broadphase pruning.
    namespace Layers
    {
        static constexpr JPH::ObjectLayer STATIC     = 0;
        static constexpr JPH::ObjectLayer MOVING     = 1;
        static constexpr JPH::ObjectLayer TRIGGER    = 2;
        static constexpr JPH::ObjectLayer NUM_LAYERS = 3;
    }

    // Broadphase layers control which BVH a body lives in. Mirroring the object layers 1:1 keeps things
    // simple at the cost of three trees; for Tier 0 scenes this is fine and the alternative
    // (NON_MOVING / MOVING split with TRIGGER folded into MOVING) costs us cheap broadphase early-outs
    // for trigger-only queries.
    namespace BroadPhaseLayers
    {
        static constexpr JPH::BroadPhaseLayer STATIC(0);
        static constexpr JPH::BroadPhaseLayer MOVING(1);
        static constexpr JPH::BroadPhaseLayer TRIGGER(2);
        static constexpr JPH::uint            NUM_LAYERS = 3;
    }

    // Maps an object layer to its broadphase layer. Trivial 1:1 mapping; the indirection exists so we
    // could later collapse e.g. STATIC and TRIGGER into the same broadphase tree without touching call
    // sites.
    class BPLayerInterfaceImpl final : public JPH::BroadPhaseLayerInterface
    {
    public:
        BPLayerInterfaceImpl()
        {
            m_Map[Layers::STATIC]  = BroadPhaseLayers::STATIC;
            m_Map[Layers::MOVING]  = BroadPhaseLayers::MOVING;
            m_Map[Layers::TRIGGER] = BroadPhaseLayers::TRIGGER;
        }

        JPH::uint GetNumBroadPhaseLayers() const override
        {
            return BroadPhaseLayers::NUM_LAYERS;
        }

        JPH::BroadPhaseLayer GetBroadPhaseLayer(JPH::ObjectLayer inLayer) const override
        {
            JPH_ASSERT(inLayer < Layers::NUM_LAYERS);
            return m_Map[inLayer];
        }

#if defined(JPH_PROFILE_ENABLED) || defined(JPH_EXTERNAL_PROFILE)
        const char* GetBroadPhaseLayerName(JPH::BroadPhaseLayer inLayer) const override
        {
            switch ((JPH::BroadPhaseLayer::Type)inLayer)
            {
                case (JPH::BroadPhaseLayer::Type)BroadPhaseLayers::STATIC.GetValue():  return "STATIC";
                case (JPH::BroadPhaseLayer::Type)BroadPhaseLayers::MOVING.GetValue():  return "MOVING";
                case (JPH::BroadPhaseLayer::Type)BroadPhaseLayers::TRIGGER.GetValue(): return "TRIGGER";
                default:                                                               return "INVALID";
            }
        }
#endif

    private:
        JPH::BroadPhaseLayer m_Map[Layers::NUM_LAYERS];
    };

    // Object-vs-broadphase early-out. Returning false here lets Jolt skip an entire BVH for a given query
    // origin layer — the cheapest filter we can apply.
    class ObjectVsBroadPhaseLayerFilterImpl final : public JPH::ObjectVsBroadPhaseLayerFilter
    {
    public:
        bool ShouldCollide(JPH::ObjectLayer inLayer1, JPH::BroadPhaseLayer inLayer2) const override
        {
            switch (inLayer1)
            {
                case Layers::STATIC:  return inLayer2 == BroadPhaseLayers::MOVING;
                case Layers::MOVING:  return true;
                case Layers::TRIGGER: return inLayer2 == BroadPhaseLayers::MOVING;
                default:              return false;
            }
        }
    };

    // Pairwise filter applied after broadphase narrows candidates. STATIC/STATIC and TRIGGER/TRIGGER are
    // dropped (no point in colliding); MOVING collides with everything; TRIGGER fires only against MOVING.
    class ObjectLayerPairFilterImpl final : public JPH::ObjectLayerPairFilter
    {
    public:
        bool ShouldCollide(JPH::ObjectLayer inLayer1, JPH::ObjectLayer inLayer2) const override
        {
            switch (inLayer1)
            {
                case Layers::STATIC:  return inLayer2 == Layers::MOVING;
                case Layers::MOVING:  return true;
                case Layers::TRIGGER: return inLayer2 == Layers::MOVING;
                default:              return false;
            }
        }
    };
}
