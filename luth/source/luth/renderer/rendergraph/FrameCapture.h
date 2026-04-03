#pragma once

#include "luth/core/LuthTypes.h"
#include "luth/renderer/rendergraph/RenderGraphSnapshot.h"

#include <glm/glm.hpp>
#include <string>
#include <vector>
#include <vulkan/vulkan.h>

namespace Luth::RG
{
    // Pipeline state captured at draw time (not hardcoded)
    struct CapturedPipelineState
    {
        std::string shaderName;
        u32  renderMode  = 0;       // Material::RenderMode as u32
        u32  cullMode    = 0;       // VK_CULL_MODE_* value
        VkPolygonMode polygonMode = VK_POLYGON_MODE_FILL;
        bool isSkinned   = false;
        bool depthTest   = false;
        bool depthWrite  = false;
        bool blendEnabled = false;
    };

    // One per vkCmdDrawIndexed / vkCmdDraw call
    struct CapturedDrawCall
    {
        u32 globalIndex     = 0;    // 0-based across entire frame
        u32 passLocalIndex  = 0;    // 0-based within its pass
        u32 passIndex       = 0;    // index into CapturedFrame::passes

        std::string passName;
        std::string meshName;       // model name + mesh index
        std::string entityName;
        u32 entityIndex  = 0;
        u32 indexCount   = 0;

        // Snapshot of the push constants at draw time
        glm::mat4 modelMatrix = glm::mat4(1.0f);
        u32 materialIndex = 0;
        u32 shadeMode     = 0;
        u32 entityID      = 0;
        u32 boneOffset    = 0;

        CapturedPipelineState pipelineState;
    };

    // Aggregated info per render pass
    struct CapturedPass
    {
        std::string name;
        u32 firstDrawIndex  = 0;    // into CapturedFrame::drawCalls
        u32 drawCallCount   = 0;
        float gpuTimeMs     = -1.0f;

        CapturedPipelineState pipelineState;

        // Active render target tracking for rescue blit
        std::string activeRenderTarget;
        bool isDepthTarget = false;
    };

    // The complete captured frame data
    struct CapturedFrame
    {
        std::vector<CapturedDrawCall> drawCalls;
        std::vector<CapturedPass>     passes;
        std::vector<ResourceSnapshot> resources;
        float totalGpuTimeMs = 0.0f;
        bool  valid          = false;

        void Clear()
        {
            drawCalls.clear();
            passes.clear();
            resources.clear();
            totalGpuTimeMs = 0.0f;
            valid = false;
        }
    };
}
