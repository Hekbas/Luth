#pragma once

#include "luth/core/LuthTypes.h"
#include "luth/core/UUID.h"

#include <entt/entt.hpp>
#include <unordered_map>

namespace Luth
{
    struct DrawList;

    // Walks the ECS once per frame and partitions WorldTransform + MeshRenderer
    // entities into a DrawList by Material::RenderMode. The builder is stateless;
    // the caller owns the DrawList instance so vectors can be reused across frames.
    //
    // Invariant: BuildGPUObjectBuffer must run first. DrawListBuilder skips any
    // entity absent from entityToSSBOIndex so dc.gpuObjectIndex / dc.entityIndex
    // always match the live GPU indirect buffer.
    class DrawListBuilder
    {
    public:
        void Build(entt::registry& registry,
                   const std::unordered_map<UUID, u32, UUIDHash>& materialSlotMap,
                   const std::unordered_map<entt::entity, u32>& entityToSSBOIndex,
                   DrawList& out);
    };
}
