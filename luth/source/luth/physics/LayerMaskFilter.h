#pragma once

#include "luth/core/types/LuthTypes.h"
#include "luth/physics/PhysicsLayers.h"

#include <Jolt/Jolt.h>
#include <Jolt/Physics/Collision/BroadPhase/BroadPhaseLayer.h>
#include <Jolt/Physics/Collision/ObjectLayer.h>

namespace Luth::Physics
{
    // u32 bitmask layout: bit N set means layer N participates in the query. Mask == 0 means "no filtering"
    // (every layer participates), the gameplay default for "hit anything". Mirrors Unity's LayerMask
    // semantics: bit position == layer index.
    //
    // Broadphase and object layers share indices 1:1 here (see BPLayerInterfaceImpl in
    // PhysicsLayers.h), so the same mask drives both filter passes.

    class LayerMaskBroadPhaseFilter final : public JPH::BroadPhaseLayerFilter
    {
    public:
        explicit LayerMaskBroadPhaseFilter(u32 mask) : m_Mask(mask) {}

        bool ShouldCollide(JPH::BroadPhaseLayer layer) const override
        {
            if (m_Mask == 0) return true;
            return (m_Mask & (1u << layer.GetValue())) != 0;
        }

    private:
        u32 m_Mask;
    };

    class LayerMaskObjectFilter final : public JPH::ObjectLayerFilter
    {
    public:
        explicit LayerMaskObjectFilter(u32 mask) : m_Mask(mask) {}

        bool ShouldCollide(JPH::ObjectLayer layer) const override
        {
            if (m_Mask == 0) return true;
            return (m_Mask & (1u << layer)) != 0;
        }

    private:
        u32 m_Mask;
    };
}
