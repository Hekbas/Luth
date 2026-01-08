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
        // The backend (Vulkan) will subclass or populate this
        void* commandBuffer = nullptr; 
        
        // Access to physical resources (void* = VkImage/VkBuffer)
        // The executor must set this callback
        std::function<void*(ResourceHandle)> GetResource;
    };

    // ===================================================================================
    // Render Graph
    // ===================================================================================

    class RenderGraph
    {
    public:
        struct PassNode
        {
            std::string name;
            std::function<void(RenderPassContext&)> execute;
            
            std::vector<ResourceHandle> reads;
            std::vector<ResourceHandle> writes;
            std::vector<Barrier> preBarriers;
        };

        struct ResourceNode
        {
            TextureDesc desc;
            u32 version = 0;
            bool isTransient = true;
            
            ResourceState initialState = ResourceState::Undefined;
            ResourceState currentState = ResourceState::Undefined;
            
            // Runtime data (filled by Executor)
            void* physicalResource = nullptr; 
        };

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
        void Execute(); // Calls the lambdas (CPU side)

        // Internal API for Builder
        ResourceHandle RegisterResource(const TextureDesc& desc);
        
        // Import an existing resource (e.g. Swapchain Image)
        ResourceHandle ImportResource(const TextureDesc& desc, void* physicalResource, ResourceState initialState);

        void RegisterRead(u32 passIndex, ResourceHandle handle);
        ResourceHandle RegisterWrite(u32 passIndex, ResourceHandle handle);

        // Accessors for the Backend Executor
        const std::vector<PassNode>& GetPasses() const { return m_Passes; }
        std::vector<ResourceNode>& GetResources() { return m_Resources; }

    private:
        LinearAllocator& m_Allocator;
        std::vector<PassNode> m_Passes;
        std::vector<ResourceNode> m_Resources;
    };
}
