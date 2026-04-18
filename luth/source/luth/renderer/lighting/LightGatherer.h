#pragma once

#include "luth/renderer/lighting/LightTypes.h"

#include <entt/entt.hpp>

namespace Luth
{
    // Walks the ECS once per frame to populate LightUniforms (one directional +
    // up to 64 point lights). The first Component::DirectionalLight also
    // contributes shadow config via DirectionalLightShadowParams.
    //
    // Shadow params are "sticky": if no directional light is present this frame
    // the out struct is left untouched so last-known values persist (matches the
    // legacy RenderingSystem cache behavior).
    class LightGatherer
    {
    public:
        void Gather(entt::registry& registry,
                    LightUniforms& outLights,
                    DirectionalLightShadowParams& outShadow) const;
    };
}
