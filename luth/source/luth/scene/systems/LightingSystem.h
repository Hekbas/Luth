#pragma once

#include "luth/scene/systems/ISystem.h"
#include "luth/renderer/CameraParams.h"
#include "luth/renderer/lighting/CascadeBuilder.h"
#include "luth/renderer/lighting/LightGatherer.h"
#include "luth/renderer/lighting/LightTypes.h"

#include <entt/entt.hpp>

namespace Luth
{
    // CPU-side per-frame light gathering + directional-light CSM cascade fit.
    // Outputs feed RenderPipeline's global UBO, shadow-cascade frustum cull,
    // and the frame debugger capturedFrame snapshot.
    //
    // Invoked from RenderingSystem::Update via UpdateFor() so the CPU flow
    // (gather -> cascade fit -> UBO upload -> global uniforms) stays sequenced.
    // The ISystem Update override is intentionally a no-op.
    class LightingSystem : public ISystem
    {
    public:
        void Update(Scene* scene) override {}

        void UpdateFor(entt::registry& registry, const CameraParams& camera);

        const LightUniforms&                GetLights()       const { return m_Lights; }
        const CascadeData&                  GetCascades()     const { return m_Cascades; }
        const DirectionalLightShadowParams& GetShadowParams() const { return m_Shadow; }

    private:
        LightGatherer                m_Gatherer;
        CascadeBuilder               m_Builder;
        LightUniforms                m_Lights{};
        CascadeData                  m_Cascades{};
        DirectionalLightShadowParams m_Shadow{};
    };
}
