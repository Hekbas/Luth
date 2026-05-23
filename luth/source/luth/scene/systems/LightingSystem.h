#pragma once

#include "luth/scene/systems/ISystem.h"
#include "luth/renderer/CameraParams.h"
#include "luth/renderer/lighting/CascadeBuilder.h"
#include "luth/renderer/lighting/FogVolumeGatherer.h"
#include "luth/renderer/lighting/LightGatherer.h"
#include "luth/renderer/lighting/LightTypes.h"

namespace Luth
{
    struct RenderSnapshot;

    // CPU-side per-frame light gathering + directional-light CSM cascade fit.
    // Outputs feed RenderPipeline's per-view LightSSBO + cluster passes, shadow-cascade frustum
    // cull, and the frame debugger capturedFrame snapshot.
    //
    // Invoked from RenderingSystem::Update via UpdateFor() so the CPU flow stays sequenced.
    // The ISystem Update override is intentionally a no-op.
    class LightingSystem : public ISystem
    {
    public:
        void Update(Scene* scene) override {}

        void UpdateFor(const RenderSnapshot& snapshot, const CameraParams& camera);

        const GatheredLights&               GetLights()       const { return m_Lights; }
        const GatheredFogVolumes&           GetFogVolumes()   const { return m_FogVolumes; }
        const CascadeData&                  GetCascades()     const { return m_Cascades; }
        const DirectionalLightShadowParams& GetShadowParams() const { return m_Shadow; }

    private:
        LightGatherer                m_Gatherer;
        FogVolumeGatherer            m_FogGatherer;
        CascadeBuilder               m_Builder;
        GatheredLights               m_Lights{};
        GatheredFogVolumes           m_FogVolumes{};
        CascadeData                  m_Cascades{};
        DirectionalLightShadowParams m_Shadow{};
    };
}
