#pragma once

#include "luth/core/types/LuthTypes.h"
#include "luth/core/UUID.h"

#include <entt/entt.hpp>
#include <unordered_map>

namespace Luth
{
    struct DrawList;
    struct RenderSnapshot;

    // Partitions the per-frame RenderSnapshot's mesh rows into a DrawList by Material::RenderMode. The builder is
    // stateless; the caller owns the DrawList instance so vectors can be reused across frames.
    //
    // Invariant: BuildGPUObjectBuffer must run first. DrawListBuilder skips any snapshot row whose entity is absent
    // from entityToSSBOIndex so dc.gpuObjectIndex / dc.entityIndex always match the live GPU indirect buffer.
    class DrawListBuilder
    {
    public:
        void Build(const RenderSnapshot& snapshot,
                   const std::unordered_map<UUID, u32, UUIDHash>& materialSlotMap,
                   const std::unordered_map<entt::entity, u32>& entityToSSBOIndex,
                   DrawList& out);
    };
}
