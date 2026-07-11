#pragma once

#include "luth/renderer/lighting/LightTypes.h"

namespace Luth
{
    struct RenderSnapshot;
    struct EmissiveLightSettings;

    // Enumerates emissive-material triangles from the frozen RenderSnapshot into world-space
    // TriangleLightData, then builds the power-weighted alias table over the UNIFIED [points | triangles]
    // local-light list. Results land in GatheredLights.tris / .alias. Runs render-side (needs the snapshot
    // + AssetManager + the ReSTIR-DI enable flag). Rebuild is skipped when the emissive-instance + point
    // power state hashes identical to last frame, so a static scene rebuilds nothing.
    // see arch/rendering-pipeline.md
    class EmissiveLightGatherer
    {
    public:
        // diEnabled = RestirSettings::enabled. Feature or DI off -> tris/alias cleared (emitters keep the
        // self-glow + GI on-hit seed via the TlasBuilder emitter bit staying unset).
        void Gather(const RenderSnapshot& snapshot, GatheredLights& lights,
                    const EmissiveLightSettings& settings, bool diEnabled);

    private:
        u64 m_LastHash = 0;   // emissive-instance + point-power state hash; skip rebuild when unchanged
    };
}
