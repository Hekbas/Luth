#include "luthpch.h"
#include "luth/renderer/SkeletonRenderer.h"
#include "luth/renderer/openGL/GLSkeletonRenderer.h"

namespace Luth
{
    std::unique_ptr<SkeletonRenderer> SkeletonRenderer::Create() {
        // Currently only OpenGL implementation
        return std::make_unique<GLSkeletonRenderer>();
    }
}
