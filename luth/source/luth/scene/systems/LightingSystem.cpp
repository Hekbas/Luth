#include "luthpch.h"
#include "luth/scene/systems/LightingSystem.h"
#include "luth/core/RenderSnapshot.h"
#include "luth/core/diagnostics/Profiler.h"

namespace Luth
{
    void LightingSystem::UpdateFor(const RenderSnapshot& snapshot, const CameraParams& camera,
                                   const EmissiveLightSettings& emissiveSettings, bool diEnabled)
    {
        LH_PROFILE_FUNCTION();
        m_Gatherer.Gather(snapshot, m_Lights, m_Shadow);
        m_FogGatherer.Gather(snapshot, m_FogVolumes);
        // Emissive-material triangles as area lights: fills m_Lights.tris + the unified alias table.
        m_EmissiveGatherer.Gather(snapshot, m_Lights, emissiveSettings, diEnabled);
        // Cascades feed two consumers: pbr.frag's cascade-PCF (CSM mode only) AND
        // volumetric_inject_scatter.comp's per-froxel sun-shadow sample (both modes). Build
        // unconditionally when shadows are on so volumetric god-rays stay correct even when
        // pbr.frag uses RT shadows. TODO: RT volumetric shadows would let RT mode skip cascades.
        if (m_Shadow.castShadows)
            m_Builder.Build(m_Lights.dirLight.direction, camera, m_Shadow, m_Cascades);
    }
}
