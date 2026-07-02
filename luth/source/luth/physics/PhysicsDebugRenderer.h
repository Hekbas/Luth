#pragma once

#include <Jolt/Jolt.h>

#ifdef JPH_DEBUG_RENDERER
#include <Jolt/Renderer/DebugRendererSimple.h>

namespace Luth::Physics
{
    // JPH::DebugRendererSimple subclass that forwards Jolt's wireframe lines into Luth's shared
    // DebugDraw facility. Owned by PhysicsSystem. Triangle calls fall through to the base class
    // which decomposes them into three lines (sufficient for wireframe shape preview); text is
    // dropped since DebugDraw has no text path yet.
    class PhysicsDebugRenderer final : public JPH::DebugRendererSimple
    {
    public:
        PhysicsDebugRenderer();

        void DrawLine(JPH::RVec3Arg from, JPH::RVec3Arg to, JPH::ColorArg color) override;
        void DrawText3D(JPH::RVec3Arg, const std::string_view&, JPH::ColorArg, float) override {}
    };
}
#endif // JPH_DEBUG_RENDERER
