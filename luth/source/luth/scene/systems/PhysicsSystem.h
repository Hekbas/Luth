#pragma once

#include "luth/scene/systems/ISystem.h"
#include "luth/physics/PhysicsLayers.h"
#include "luth/physics/LuthJobSystemForJolt.h"

#include <Jolt/Jolt.h>

#include <Jolt/Core/TempAllocator.h>
#include <Jolt/Physics/PhysicsSystem.h>

namespace Luth
{
    class Scene;

    // Drives the JPH::PhysicsSystem instance for the active scene. Owns the broadphase/object layer
    // filters, the LuthJobSystemForJolt adapter, and the temp allocator. Update() steps the simulation
    // under a fixed-step accumulator gated on PlayState — Editing pauses the sim, Playing/Paused-with-step
    // ticks it. This is the skeleton: body lifecycle, queries, and event delivery land in follow-ups.
    class PhysicsSystem final : public ISystem
    {
    public:
        PhysicsSystem();
        ~PhysicsSystem() override;

        void Update(Scene* scene) override;

    private:
        void Step(f32 fixedDt, int collisionSteps);

        // Order matters: m_TempAlloc and m_JobAdapter are referenced by m_System.Update() each step, and
        // member destruction order is reverse declaration. Putting m_System last ensures it tears down
        // before its dependencies.
        Physics::BPLayerInterfaceImpl              m_BPLayers;
        Physics::ObjectVsBroadPhaseLayerFilterImpl m_OvBpFilter;
        Physics::ObjectLayerPairFilterImpl         m_LayerPairFilter;
        JPH::TempAllocatorImpl                     m_TempAlloc;
        Physics::LuthJobSystemForJolt              m_JobAdapter;
        JPH::PhysicsSystem                         m_System;

        f32 m_Accumulator = 0.0f;

        static constexpr f32 kFixedDt     = 1.0f / 60.0f;
        static constexpr int kMaxSubSteps = 4;

        // Body manager capacities — high enough for typical Tier 0 scenes; promotable to runtime config
        // once we have data on what real scenes need.
        static constexpr JPH::uint kMaxBodies              = 16384;
        static constexpr JPH::uint kNumBodyMutexes         = 0;
        static constexpr JPH::uint kMaxBodyPairs           = 65536;
        static constexpr JPH::uint kMaxContactConstraints  = 16384;

        static constexpr JPH::uint kTempAllocatorBytes     = 32 * 1024 * 1024;
        static constexpr JPH::uint kAdapterMaxJobs         = 2048;
        static constexpr JPH::uint kAdapterMaxBarriers     = 8;
    };
}
