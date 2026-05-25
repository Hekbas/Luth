#pragma once

#include "luth/core/types/LuthMath.h"

#include <vector>

namespace Luth
{
    struct RenderSnapshot;

    // std430-compatible row in the FogVolume SSBO. worldToVolume is inverted from the snapshot's
    // worldMatrix at gather time so the injection shader transforms world_pos with a single
    // multiply. extentsOrRadius packs the active union member (Box halfExtents vs Sphere radius
    // in .x) and `type` selects interpretation.
    struct FogVolumeData
    {
        Mat4 worldToVolume;     // 64
        Vec3 extentsOrRadius;   // 12
        u32  type;              //  4   80
        Vec3 color;             // 12
        f32  density;           //  4   96
        f32  falloffStart;      //  4
        f32  falloffEnd;        //  4
        u32  affectsAmbient;    //  4
        u32  _pad;              //  4  112
    };
    static_assert(sizeof(FogVolumeData) == 112, "FogVolumeData std430 layout");

    // CPU-side aggregate produced by FogVolumeGatherer each game stage.
    struct GatheredFogVolumes
    {
        std::vector<FogVolumeData> volumes;
    };

    // SSBO layout: { FogVolumeSSBOHeader header; FogVolumeData volumes[header.count]; }
    struct FogVolumeSSBOHeader
    {
        u32 count;
        u32 _pad[3];
    };
    static_assert(sizeof(FogVolumeSSBOHeader) == 16, "FogVolumeSSBOHeader std430 layout");

    class FogVolumeGatherer
    {
    public:
        void Gather(const RenderSnapshot& snapshot, GatheredFogVolumes& out) const;
    };
}
