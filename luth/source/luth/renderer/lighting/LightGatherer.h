#pragma once

#include "luth/renderer/lighting/LightTypes.h"

namespace Luth
{
    struct RenderSnapshot;

    // Translates the per-frame RenderSnapshot's light rows into a GatheredLights aggregate
    // (one directional + an unbounded point-light vector) plus DirectionalLightShadowParams.
    // Shadow params are "sticky" — if no directional light is present this frame, the out
    // struct is left untouched so last-known values persist.
    class LightGatherer
    {
    public:
        void Gather(const RenderSnapshot& snapshot,
                    GatheredLights& outLights,
                    DirectionalLightShadowParams& outShadow) const;
    };
}
