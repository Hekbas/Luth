#include "luthpch.h"
#include "luth/scene/systems/LightingSystem.h"
#include "luth/core/diagnostics/Profiler.h"

namespace Luth
{
    void LightingSystem::UpdateFor(entt::registry& registry, const CameraParams& camera)
    {
        LH_PROFILE_FUNCTION();
        m_Gatherer.Gather(registry, m_Lights, m_Shadow);
        m_Builder.Build(m_Lights.dirLight.direction, camera, m_Shadow, m_Cascades);
    }
}
