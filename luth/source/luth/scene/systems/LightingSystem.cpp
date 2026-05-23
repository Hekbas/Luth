#include "luthpch.h"
#include "luth/scene/systems/LightingSystem.h"
#include "luth/core/RenderSnapshot.h"
#include "luth/core/diagnostics/Profiler.h"

namespace Luth
{
    void LightingSystem::UpdateFor(const RenderSnapshot& snapshot, const CameraParams& camera)
    {
        LH_PROFILE_FUNCTION();
        m_Gatherer.Gather(snapshot, m_Lights, m_Shadow);
        m_FogGatherer.Gather(snapshot, m_FogVolumes);
        m_Builder.Build(m_Lights.dirLight.direction, camera, m_Shadow, m_Cascades);
    }
}
