#pragma once

#include "luth/core/types/LuthTypes.h"

namespace Luth
{
    // Sorted = classic per-view back-to-front alpha blend (per-mesh order — overlap/interpenetration
    // can still misorder per-pixel). OIT = per-pixel linked-list store + sorted resolve (exact up to
    // the node budget). Both run in the same pass slot after the volumetric composite.
    enum class TransparencyMode : i32 { Sorted = 0, OIT = 1 };

    // Read each frame by TransparencySubsystem. avgLayersBudget sizes the per-view OIT node pool
    // (W × H × budget × 16 B — a VRAM knob, overflow drops fragments); maxResolveK caps how many
    // nearest fragments the resolve exact-sorts per pixel (extras tail-merge into the farthest slot).
    struct TransparencySettings
    {
        TransparencyMode mode = TransparencyMode::OIT;
        u32 avgLayersBudget = 4;   // node pool budget, average layers per pixel (1-16)
        u32 maxResolveK     = 8;   // exact-sorted layers per pixel in the resolve (1-16)
    };
}
