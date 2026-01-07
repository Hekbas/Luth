#pragma once

#include "luth/renderer/rendergraph/RenderGraphResources.h"
#include "luth/core/Memory.h"
#include "luth/core/JobSystem.h"

#include <vector>
#include <functional>
#include <string>
#include <unordered_map>

namespace Luth::RG
{
    class RenderGraph;

    // ===================================================================================
    // Pass Builder
    // ===================================================================================

    class RenderPassBuilder
    {
    public:
        RenderPassBuilder(RenderGraph& graph, u32 passIndex)
            : m_Graph(graph), m_PassIndex(passIndex) {}

        ResourceHandle Read(ResourceHandle resource);
        ResourceHandle Write(ResourceHandle resource);
        ResourceHandle CreateTexture(const TextureDesc& desc);

    private:
        RenderGraph& m_Graph;
        u32 m_PassIndex;
    };

    // ===================================================================================
    // Pass Execution Context
    // ===================================================================================

    class RenderPassContext
    {
    public:
        // TODO: Add methods to get actual Vulkan objects
        // void* GetTexture(ResourceHandle handle);
        // VkCommandBuffer GetCommandBuffer();
    };

    // ===================================================================================
    // Render Graph
    // ===================================================================================

    class RenderGraph
    {
    public:
        RenderGraph(LinearAllocator& allocator);
        ~RenderGraph() = default;

        template<typename Data, typename SetupFunc, typename ExecuteFunc>
        void AddPass(const std::string& name, SetupFunc&& setup, ExecuteFunc&& execute)
        {
            Data* data = m_Allocator.New<Data>();

            RenderPassBuilder builder(*this, (u32)m_Passes.size());
            setup(*data, builder);

            auto executeWrapper = [execute, data](RenderPassContext& ctx) {
                execute(*data, ctx);
            };

            m_Passes.push_back({ name, executeWrapper });
        }

        void Compile();
        void Execute();

        // Internal API for Builder
        ResourceHandle RegisterResource(const TextureDesc& desc);
        void RegisterRead(u32 passIndex, ResourceHandle handle);
        ResourceHandle RegisterWrite(u32 passIndex, ResourceHandle handle);

    private:
        struct PassNode
        {
            std::string name;
            std::function<void(RenderPassContext&)> execute;
            
            std::vector<ResourceHandle> reads;
            std::vector<ResourceHandle> writes;
            
            // Barriers to execute BEFORE this pass starts
            std::vector<Barrier> preBarriers;
        };

        struct ResourceNode
        {
            TextureDesc desc;
            u32 version = 0;
            bool isTransient = true;
            
            // State tracking for compiler
            ResourceState initialState = ResourceState::Undefined;
            ResourceState currentState = ResourceState::Undefined;
        };

        LinearAllocator& m_Allocator;
        std::vector<PassNode> m_Passes;
        std::vector<ResourceNode> m_Resources;
    };
}
