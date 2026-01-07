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

    /**
     * @brief Interface exposed to the Pass Setup lambda.
     * Allows the pass to declare its inputs and outputs.
     */
    class RenderPassBuilder
    {
    public:
        RenderPassBuilder(RenderGraph& graph, u32 passIndex)
            : m_Graph(graph), m_PassIndex(passIndex) {}

        /**
         * @brief Declares that this pass reads from a resource.
         * @return The handle to the resource (versioned).
         */
        ResourceHandle Read(ResourceHandle resource);

        /**
         * @brief Declares that this pass writes to a resource.
         * This effectively creates a new version of the resource.
         * @return The new handle representing the resource AFTER this write.
         */
        ResourceHandle Write(ResourceHandle resource);

        /**
         * @brief Creates a new transient texture managed by the graph.
         */
        ResourceHandle CreateTexture(const TextureDesc& desc);

        // TODO: CreateBuffer, ReadBuffer, WriteBuffer

    private:
        RenderGraph& m_Graph;
        u32 m_PassIndex;
    };

    // ===================================================================================
    // Pass Execution Context
    // ===================================================================================

    /**
     * @brief Interface exposed to the Pass Execute lambda.
     * Provides access to the actual GPU resources (VkImage, VkBuffer).
     */
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

        /**
         * @brief Adds a render pass to the graph.
         * 
         * @tparam Data A struct to hold pass-specific data (handles, settings).
         * @param name Name of the pass for debugging.
         * @param setup Lambda called immediately to define inputs/outputs.
         * @param execute Lambda called during graph execution to record commands.
         */
        template<typename Data, typename SetupFunc, typename ExecuteFunc>
        void AddPass(const std::string& name, SetupFunc&& setup, ExecuteFunc&& execute)
        {
            // Allocate the pass data in the frame allocator
            Data* data = m_Allocator.New<Data>();

            // Setup phase
            RenderPassBuilder builder(*this, (u32)m_Passes.size());
            setup(*data, builder);

            // Store the pass
            auto executeWrapper = [execute, data](RenderPassContext& ctx) {
                execute(*data, ctx);
            };

            m_Passes.push_back({ name, executeWrapper });
        }

        /**
         * @brief Compiles the graph.
         * Calculates dependencies, culls unused passes, and injects barriers.
         */
        void Compile();

        /**
         * @brief Executes the graph.
         * Dispatches pass recording to the JobSystem and submits to the GPU.
         */
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
            
            // Barrier info calculated during Compile()
            // std::vector<Barrier> preBarriers;
        };

        struct ResourceNode
        {
            TextureDesc desc;
            u32 version = 0;
            bool isTransient = true;
            // void* gpuResource = nullptr; // Pointer to actual VkImage
        };

        LinearAllocator& m_Allocator;
        std::vector<PassNode> m_Passes;
        std::vector<ResourceNode> m_Resources;
    };
}
