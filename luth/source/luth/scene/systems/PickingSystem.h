#pragma once

#include "luth/scene/systems/ISystem.h"
#include "luth/core/types/LuthMath.h"

#include <entt/entt.hpp>

namespace Luth
{
    // Mouse-picking readback. The renderer always writes an EntityID texture
    // during GeometryPass; this system samples a single pixel from it on
    // demand and maps the value back to an entt::entity via the Pipeline's
    // EntityLookup table.
    //
    // Editor panels call RequestPick(x, y) from the input handler, then
    // poll HasResult() / ConsumeResult() on a later frame. Update() runs
    // after RenderingSystem so the EntityID buffer has valid contents.
    class PickingSystem : public ISystem
    {
    public:
        void Update(Scene* scene) override;

        void RequestPick(int x, int y);
        bool HasResult() const { return m_Ready; }
        entt::entity ConsumeResult();

    private:
        bool         m_Pending = false;
        bool         m_Ready   = false;
        IVec2        m_Coord   = { 0, 0 };
        entt::entity m_Picked  = entt::null;
    };
}
