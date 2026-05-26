#pragma once

#include "luth/core/types/LuthTypes.h"

namespace Luth
{
    class RenderPipeline;

    // Houses RT-domain state across the rt-renderer arc. B.1 only hosts a validation-gated
    // smoke test (compile a raygen, build a pipeline + SBT, traceRays(1,1,1), destroy);
    // B.2 brings TLAS + per-frame rebuild; B.3 brings the production RT shadow pipeline.
    // No per-frame state, no descriptor sets, no Add*Pass in B.1 — the subsystem exists
    // so future work has a stable home.
    class RtSubsystem
    {
    public:
        void Init(RenderPipeline& pipeline);
        void Shutdown();

    private:
        RenderPipeline* m_Pipeline = nullptr;
    };
}
