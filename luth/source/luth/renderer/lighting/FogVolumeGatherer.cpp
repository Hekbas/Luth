#include "luthpch.h"
#include "luth/renderer/lighting/FogVolumeGatherer.h"
#include "luth/core/RenderSnapshot.h"

namespace Luth
{
    void FogVolumeGatherer::Gather(const RenderSnapshot& snapshot, GatheredFogVolumes& out) const
    {
        LH_PROFILE_FUNCTION();

        const u32 count = static_cast<u32>(snapshot.fogVolumes.size());
        out.volumes.resize(count);
        for (u32 i = 0; i < count; ++i)
        {
            const auto& src = snapshot.fogVolumes[i];
            FogVolumeData& dst = out.volumes[i];
            dst.worldToVolume   = Math::Inverse(src.worldMatrix);
            dst.extentsOrRadius = src.extentsOrRadius;
            dst.type            = src.type;
            dst.color           = src.color;
            dst.density         = src.density;
            dst.falloffStart    = src.falloffStart;
            dst.falloffEnd      = src.falloffEnd;
            dst.affectsAmbient  = src.affectsAmbient ? 1u : 0u;
            dst._pad            = 0u;
        }
    }
}
