#pragma once

#include "luth/core/types/LuthTypes.h"
#include "luth/renderer/rendergraph/RenderGraphResources.h"

#include <vulkan/vulkan.h>

namespace Luth::RG
{
    class RenderGraph;

    // Hook invoked by RenderGraph::Execute after each non-culled pass finishes recording. The sink may emit transfer
    // copies of the pass's written render targets into its own staging images for later inspection (Frame Debugger).
    //
    // Contract:
    //   - The sink MUST restore the source RT's layout to whatever the pass wrote it as
    //     (i.e. ColorAttachmentOptimal / DepthStencilAttachmentOptimal). The RG's compile-time barrier
    //     solver is unaware of this hook, so any layout change leaked back to the next pass will desync barriers.
    //   - The sink owns its destination images entirely (no RG tracking).
    //   - May be called multiple times per frame across multiple passes.
    //   - When queueFamily is AsyncCompute, the sink MUST emit only stages valid on a compute queue
    //     (no FRAGMENT_SHADER_BIT, no COLOR_ATTACHMENT_OUTPUT_BIT, etc.); substitute with COMPUTE_SHADER_BIT
    //     or TOP_OF_PIPE_BIT. vkCmdCopyImage / vkCmdPipelineBarrier2 are valid on both queues per spec.
    class IArchiveSink
    {
    public:
        virtual ~IArchiveSink() = default;

        virtual void OnPassExecuted(u32 passIndex, RenderGraph& graph, VkCommandBuffer primaryCmd,
                                    QueueFamily queueFamily) = 0;
    };
}
