#include "luthpch.h"

#include "luth/physics/PhysicsDebugRenderer.h"

#ifdef JPH_DEBUG_RENDERER

#include "luth/core/DebugDraw.h"
#include "luth/physics/JoltMath.h"

namespace Luth::Physics
{
    PhysicsDebugRenderer::PhysicsDebugRenderer()
    {
        // Required by Jolt's DebugRenderer base: sets up the geometry caches the base class maintains for shape
        // batching and wires the global instance pointer that some Jolt internals reference. The cache goes unused
        // (CreateTriangleBatch routes through the base's three-lines fallback), but Initialize() is mandatory.
        Initialize();
    }

    void PhysicsDebugRenderer::DrawLine(JPH::RVec3Arg from, JPH::RVec3Arg to, JPH::ColorArg color)
    {
        // JPH::Color stores RGBA8 little-endian (byte 0 = R, byte 3 = A); same byte order as VK_FORMAT_R8G8B8A8_UNORM,
        // so passing the packed u32 through is correct.
        Luth::DebugDraw::Line(FromJolt(from), FromJolt(to), color.GetUInt32());
    }
}
#endif // JPH_DEBUG_RENDERER
