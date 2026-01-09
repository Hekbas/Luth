#pragma once

#include "luth/renderer/rendergraph/RenderGraphResources.h"
#include "luth/core/Memory.h"
#include "luth/core/JobSystem.h"

#include <vulkan/vulkan.h>
#include <vector>
#include <functional>
#include <string>
#include <unordered_map>

// Forward declare VMA struct
struct VmaAllocation_T;

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
        ResourceHandle ReadTransfer(ResourceHandle resource); // New: For Blit/Copy source
        
        // Standard Write (Render Target)
        ResourceHandle Write(ResourceHandle resource);
        
        // Transfer Write (Clear/Copy)
        ResourceHandle WriteTransfer(ResourceHandle resource);

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
        VkCommandBuffer commandBuffer = VK_NULL_HANDLE;
        
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
            std::vector<ResourceState> readStates; // Track desired state for reads

            std::vector<ResourceHandle> writes;
            std::vector<ResourceState> writeStates; 

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
            VkImage image = VK_NULL_HANDLE;
            VkImageView view = VK_NULL_HANDLE;
            VmaAllocation_T* allocation = nullptr;
            bool external = false; // If true, we don't destroy it (e.g. Swapchain Image)
        };

    public:
        RenderGraph(LinearAllocator& allocator);
        ~RenderGraph() = default;

        template<typename Data, typename SetupFunc, typename ExecuteFunc>
        void AddPass(const std::string& name, SetupFunc&& setup, ExecuteFunc&& execute)
        {
            // 1. Allocate Pass Data
            Data* data = m_Allocator.New<Data>();

            // 2. Create Pass Node immediately so Builder can access it
            u32 passIndex = (u32)m_Passes.size();
            m_Passes.emplace_back();
            PassNode& node = m_Passes.back();
            node.name = name;

            // 3. Run Setup (Populates reads/writes in node)
            RenderPassBuilder builder(*this, passIndex);
            setup(*data, builder);

            // 4. Set Execute Function
            node.execute = [execute, data](RenderPassContext& ctx) {
                execute(*data, ctx);
            };
        }

        void Compile();
        void Execute(VkCommandBuffer cmd);

        // Internal API for Builder
        ResourceHandle RegisterResource(const TextureDesc& desc);
        
        // Import an existing resource (e.g. Swapchain Image)
        ResourceHandle ImportResource(const TextureDesc& desc, void* image, void* view, ResourceState initialState);

        void RegisterRead(u32 passIndex, ResourceHandle handle, ResourceState state);
        ResourceHandle RegisterWrite(u32 passIndex, ResourceHandle handle, ResourceState state);

        // Accessors for the Backend Executor
        const std::vector<PassNode>& GetPasses() const { return m_Passes; }
        std::vector<ResourceNode>& GetResources() { return m_Resources; }

    private:
        LinearAllocator& m_Allocator;
        std::vector<PassNode> m_Passes;
        std::vector<ResourceNode> m_Resources;

        void AllocatePhysicalResources();
        void CleanupPhysicalResources();
    };
}
