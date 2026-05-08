#include "luthpch.h"

#include "luth/scene/systems/PhysicsSystem.h"

#include "luth/core/EditorHooks.h"
#include "luth/core/diagnostics/Log.h"
#include "luth/core/diagnostics/Profiler.h"
#include "luth/core/time/Time.h"

namespace Luth
{
    PhysicsSystem::PhysicsSystem()
        : m_TempAlloc(kTempAllocatorBytes)
        , m_JobAdapter(kAdapterMaxJobs, kAdapterMaxBarriers)
    {
        m_System.Init(kMaxBodies, kNumBodyMutexes, kMaxBodyPairs, kMaxContactConstraints,
                      m_BPLayers, m_OvBpFilter, m_LayerPairFilter);
        LH_CORE_INFO("PhysicsSystem initialized: {} max bodies, {} pairs, {} contacts",
                     kMaxBodies, kMaxBodyPairs, kMaxContactConstraints);
    }

    PhysicsSystem::~PhysicsSystem() = default;

    void PhysicsSystem::Update(Scene* /*scene*/)
    {
        LH_PROFILE_FUNCTION();

        // Editor gate. When no editor is registered (runtime-only build) Get() is null and the sim ticks
        // unconditionally. Paused + ConsumeStepRequest() to advance one step lands alongside body
        // lifecycle, when there is something visible to step.
        if (auto* hooks = EditorHooks::Get();
            hooks && hooks->GetPlayState() == PlayState::Editing)
        {
            m_Accumulator = 0.0f;
            return;
        }

        // Fixed-step accumulator. Capped sub-steps avoid spiral-of-death when a frame stalls and dt
        // catches up; the residual stays in m_Accumulator and applies on the next frame.
        m_Accumulator += Time::DeltaTime();

        int substeps = 0;
        while (m_Accumulator >= kFixedDt && substeps < kMaxSubSteps)
        {
            Step(kFixedDt, /*collisionSteps*/1);
            m_Accumulator -= kFixedDt;
            ++substeps;
        }
    }

    void PhysicsSystem::Step(f32 dt, int collisionSteps)
    {
        LH_PROFILE_FUNCTION();
        m_System.Update(dt, collisionSteps, &m_TempAlloc, &m_JobAdapter);
    }
}
