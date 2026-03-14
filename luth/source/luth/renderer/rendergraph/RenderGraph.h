#pragma once

#include "luth/renderer/rendergraph/RenderGraphResources.h"
#include "luth/memory/Memory.h"
#include "luth/jobs/JobSystem.h"
#include "luth/renderer/backend/vulkan/DynamicRendering.h"

#include <vulkan/vulkan.h>
#include <vector>
#include <functional>
#include <string>

// Forward declare VMA struct
struct VmaAllocation_T;

namespace Luth::RG
{
    class RenderGraph;

    // ===================================================================================
    // Pass Builder — Declares resource reads/writes during setup
    // ===================================================================================

    class RenderPassBuilder
    {
    public:
        RenderPassBuilder(RenderGraph& graph, u32 passIndex)
            : m_Graph(graph), m_PassIndex(passIndex) {}

        ResourceHandle Read(ResourceHandle resource);
        ResourceHandle ReadTransfer(ResourceHandle resource);
        
        ResourceHandle Write(ResourceHandle resource, 
                             VkAttachmentLoadOp loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR, 
                             VkAttachmentStoreOp storeOp = VK_ATTACHMENT_STORE_OP_STORE, 
                             VkClearValue clearValue = {});
        
        ResourceHandle WriteDepth(ResourceHandle resource, 
                             VkAttachmentLoadOp loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR, 
                             VkAttachmentStoreOp storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE, 
                             VkClearValue clearValue = {});

        ResourceHandle WriteTransfer(ResourceHandle resource);
        ResourceHandle CreateTexture(const TextureDesc& desc);

    private:
        RenderGraph& m_Graph;
        u32 m_PassIndex;
    };

    // ===================================================================================
    // Pass Execution Context — Passed to pass execute lambdas
    // ===================================================================================

    class RenderPassContext
    {
    public:
        VkCommandBuffer commandBuffer = VK_NULL_HANDLE;
        std::function<void*(ResourceHandle)> GetResource;
    };

    // ===================================================================================
    // Render Graph — DAG Compile → Barrier Inject → Execute
    // ===================================================================================
    //
    // Execution model (Phase 3):
    //   Iterate sorted passes in order. For each pass:
    //     1. Emit batched pre-barriers into primary cmd
    //     2. BeginRendering on primary cmd
    //     3. Dispatch pass recording as a RenderPassJob (secondary cmd buffer)
    //     4. WaitForCounter (inline execute if depth allows, V5)
    //     5. ExecuteCommands (secondary into primary)
    //     6. EndRendering
    //
    //   Parallelism comes from WITHIN a pass (splitting draw calls across N jobs).
    //   Multi-pass parallelism is NOT supported (inter-pass barriers are serial).

    class RenderGraph
    {
    public:
        struct PassAttachment
        {
            ResourceHandle handle;
            VkAttachmentLoadOp loadOp;
            VkAttachmentStoreOp storeOp;
            VkClearValue clearValue;
        };

        struct PassNode
        {
            std::string name;
            std::function<void(RenderPassContext&)> execute;
            
            std::vector<ResourceHandle> reads;
            std::vector<ResourceState> readStates;

            std::vector<ResourceHandle> writes;
            std::vector<ResourceState> writeStates; 
            
            // Metadata for Dynamic Rendering
            std::vector<PassAttachment> colorAttachments;
            PassAttachment depthAttachment;
            bool hasDepth = false;

            std::vector<Barrier> preBarriers;

            // Compile output
            bool culled = false;    // Dead-pass culling 
            u32 sortOrder = 0;      // Topological order
        };

        struct ResourceNode
        {
            TextureDesc desc;
            u32 version = 0;
            bool isTransient = true;
            
            ResourceState initialState = ResourceState::Undefined;
            ResourceState currentState = ResourceState::Undefined;
            
            // Physical resource (filled by AllocatePhysicalResources)
            VkImage image = VK_NULL_HANDLE;
            VkImageView view = VK_NULL_HANDLE;
            VmaAllocation_T* allocation = nullptr;
            bool external = false;

            // Lifetime tracking (for future aliasing)
            u32 firstPass = UINT32_MAX;   // First pass that reads/writes this resource
            u32 lastPass = 0;             // Last pass that reads/writes this resource
        };

    public:
        RenderGraph(Memory::LinearAllocator& allocator);
        ~RenderGraph() = default;

        template<typename Data, typename SetupFunc, typename ExecuteFunc>
        void AddPass(const std::string& name, SetupFunc&& setup, ExecuteFunc&& execute)
        {
            Data* data = m_Allocator.New<Data>();

            u32 passIndex = (u32)m_Passes.size();
            m_Passes.emplace_back();
            PassNode& node = m_Passes.back();
            node.name = name;

            RenderPassBuilder builder(*this, passIndex);
            setup(*data, builder);

            node.execute = [execute, data](RenderPassContext& ctx) {
                execute(*data, ctx);
            };
        }

        // Compile: Cull → Barrier Solve (topo sort is implicit — passes added in order)
        void Compile();
        
        // Execute: Serial pass iteration with parallel inner recording
        void Execute(VkCommandBuffer primaryCmd);

        // Internal API for Builder
        ResourceHandle RegisterResource(const TextureDesc& desc);
        ResourceHandle ImportResource(const TextureDesc& desc, void* image, void* view, ResourceState initialState);
        void RegisterRead(u32 passIndex, ResourceHandle handle, ResourceState state);
        ResourceHandle RegisterWrite(u32 passIndex, ResourceHandle handle, ResourceState state);
        void RegisterColorAttachment(u32 passIndex, ResourceHandle handle, VkAttachmentLoadOp loadOp, VkAttachmentStoreOp storeOp, VkClearValue clearValue);
        void RegisterDepthAttachment(u32 passIndex, ResourceHandle handle, VkAttachmentLoadOp loadOp, VkAttachmentStoreOp storeOp, VkClearValue clearValue);

        // Accessors
        const std::vector<PassNode>& GetPasses() const { return m_Passes; }
        std::vector<ResourceNode>& GetResources() { return m_Resources; }

    private:
        Memory::LinearAllocator& m_Allocator;
        std::vector<PassNode> m_Passes;
        std::vector<ResourceNode> m_Resources;

        void AllocatePhysicalResources();
        void CleanupPhysicalResources();

        // Compile sub-steps
        void CullDeadPasses();
        void SolveBarriers();
        void ComputeLifetimes();
    };
}
