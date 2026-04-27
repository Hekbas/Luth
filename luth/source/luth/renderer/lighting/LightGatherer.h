#pragma once

#include "luth/renderer/lighting/LightTypes.h"

namespace Luth
{
    struct RenderSnapshot;

    // Translates the per-frame RenderSnapshot's light rows into LightUniforms
    // (one directional + up to 64 point lights). The directional row also
    // contributes shadow config via DirectionalLightShadowParams.
    //
    // Shadow params are "sticky": if no directional light is present this frame
    // the out struct is left untouched so last-known values persist (matches the
    // legacy RenderingSystem cache behavior).
    class LightGatherer
    {
    public:
        void Gather(const RenderSnapshot& snapshot,
                    LightUniforms& outLights,
                    DirectionalLightShadowParams& outShadow) const;
    };
}
