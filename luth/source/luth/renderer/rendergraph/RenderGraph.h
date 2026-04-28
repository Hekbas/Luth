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

namespace Luth { class GPUTimerPool; }

namespace Luth::RG
{
    class RenderGraph;
    class IArchiveSink;

    // ── Pass Builder — declares resource reads/writes during setup ──

    class RenderPassBuilder
    {
    public:
        RenderPassBuilder(RenderGraph& graph, u32 passIndex)
            : m_Graph(graph), m_PassIndex(passIndex) {}

        // Image resources
        ResourceHandle Read(ResourceHandle resource);
        ResourceHandle ReadTransfer(ResourceHandle resource);
        ResourceHandle ReadStorageImage(ResourceHandle resource);   // Compute read (sampled)
        ResourceHandle WriteStorageImage(ResourceHandle resource);  // Compute write (storage)

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

        // Buffer resources
        BufferHandle ReadBuffer(BufferHandle buffer);          // StorageBufferRead
        BufferHandle WriteBuffer(BufferHandle buffer);         // StorageBufferWrite
        BufferHandle ReadIndirectBuffer(BufferHandle buffer);  // IndirectRead

    private:
        RenderGraph& m_Graph;
        u32 m_PassIndex;
    };

    // ── Pass Execution Context — passed to pass execute lambdas ──

    class RenderPassContext
    {
    public:
        VkCommandBuffer commandBuffer = VK_NULL_HANDLE;
        // Graph index of the executing pass. FrameDebugger keys its sparse
        // passArchives against this; lambdas pass it to BeginCapturePass so
        // CapturedPass.graphPassIndex aligns with the archive sink's writes.
        u32 passIndex = 0;
        std::function<void*(ResourceHandle)> GetResource;
        std::function<void*(BufferHandle)> GetBuffer;
    };

    // ── Render Graph — DAG compile → barrier inject → execute ──
    //
    // Per pass: batched pre-barriers → BeginRendering → dispatch RenderPassJob
    // (secondary cmd buffer) → WaitForCounter (V5 inline execute if depth allows)
    // → ExecuteCommands → EndRendering. Parallelism is intra-pass (split draws
    // across N jobs); inter-pass barriers run serially.

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
            bool isCompute = false;  // Compute passes skip BeginRendering and secondary cmd

            // Image resource dependencies
            std::vector<ResourceHandle> reads;
            std::vector<ResourceState> readStates;

            std::vector<ResourceHandle> writes;
            std::vector<ResourceState> writeStates;

            // Buffer resource dependencies
            std::vector<BufferHandle> bufferReads;
            std::vector<ResourceState> bufferReadStates;
            std::vector<BufferHandle> bufferWrites;
            std::vector<ResourceState> bufferWriteStates;

            // Metadata for Dynamic Rendering (graphics passes only)
            std::vector<PassAttachment> colorAttachments;
            PassAttachment depthAttachment;
            bool hasDepth = false;

            std::vector<Barrier>       preBarriers;        // Image barriers
            std::vector<BufferBarrier> bufferPreBarriers;  // Buffer barriers

            // Compile output
            bool culled = false;
            u32 sortOrder = 0;
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

            // Subresource range for barrier emission. Defaults cover a single layer/mip.
            // For per-layer imports (e.g., shadow cascade), specify baseArrayLayer/layerCount
            // via the corresponding ImportResource overload so barriers target the right layer.
            u32 baseArrayLayer = 0;
            u32 layerCount     = 1;

            // Lifetime tracking (for future aliasing)
            u32 firstPass = UINT32_MAX;
            u32 lastPass = 0;
        };

        struct BufferNode
        {
            BufferDesc desc;
            u32 version = 0;
            bool isTransient = true;

            ResourceState initialState = ResourceState::Undefined;
            ResourceState currentState = ResourceState::Undefined;

            // Physical resource (filled by AllocatePhysicalResources)
            VkBuffer buffer = VK_NULL_HANDLE;
            VmaAllocation_T* allocation = nullptr;
            bool external = false;

            // Lifetime tracking
            u32 firstPass = UINT32_MAX;
            u32 lastPass = 0;
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

        // Compute passes: execute directly on primary cmd, no BeginRendering/secondary cmd
        template<typename Data, typename SetupFunc, typename ExecuteFunc>
        void AddComputePass(const std::string& name, SetupFunc&& setup, ExecuteFunc&& execute)
        {
            Data* data = m_Allocator.New<Data>();

            u32 passIndex = (u32)m_Passes.size();
            m_Passes.emplace_back();
            PassNode& node = m_Passes.back();
            node.name = name;
            node.isCompute = true;

            RenderPassBuilder builder(*this, passIndex);
            setup(*data, builder);

            node.execute = [execute, data](RenderPassContext& ctx) {
                execute(*data, ctx);
            };
        }

        // Compile: Cull → Barrier Solve (topo sort is implicit — passes added in order)
        void Compile();

        // Execute: Serial pass iteration with parallel inner recording
        void Execute(VkCommandBuffer primaryCmd, Luth::GPUTimerPool* timers = nullptr);

        // Internal API for Builder — Image resources
        ResourceHandle RegisterResource(const TextureDesc& desc);
        ResourceHandle ImportResource(const TextureDesc& desc, void* image, void* view, ResourceState initialState);
        ResourceHandle ImportResource(const TextureDesc& desc, void* image, void* view, ResourceState initialState,
                                       u32 baseArrayLayer, u32 layerCount);
        void RegisterRead(u32 passIndex, ResourceHandle handle, ResourceState state);
        ResourceHandle RegisterWrite(u32 passIndex, ResourceHandle handle, ResourceState state);
        void RegisterColorAttachment(u32 passIndex, ResourceHandle handle, VkAttachmentLoadOp loadOp, VkAttachmentStoreOp storeOp, VkClearValue clearValue);
        void RegisterDepthAttachment(u32 passIndex, ResourceHandle handle, VkAttachmentLoadOp loadOp, VkAttachmentStoreOp storeOp, VkClearValue clearValue);

        // Internal API for Builder — Buffer resources
        BufferHandle RegisterBuffer(const BufferDesc& desc);
        BufferHandle ImportBuffer(const BufferDesc& desc, void* buffer, ResourceState initialState);
        void RegisterBufferRead(u32 passIndex, BufferHandle handle, ResourceState state);
        BufferHandle RegisterBufferWrite(u32 passIndex, BufferHandle handle, ResourceState state);

        // Accessors
        const std::vector<PassNode>& GetPasses() const { return m_Passes; }
        std::vector<ResourceNode>& GetResources() { return m_Resources; }
        std::vector<BufferNode>& GetBuffers() { return m_Buffers; }

        // Archive sink — invoked after each non-culled pass during Execute. Optional.
        // The sink is responsible for restoring source RT layouts (see IArchiveSink.h).
        void SetArchiveSink(IArchiveSink* sink) { m_ArchiveSink = sink; }

        // When set, Execute uses per-pass dispatch+wait instead of the
        // default parallel two-phase. Caller sets this when pass lambdas
        // touch shared mutable state that isn't thread-safe — primary
        // case is FrameDebugger capture (passes push into shared vectors).
        void SetSerialize(bool enable) { m_SerializeDispatch = enable; }

    private:
        Memory::LinearAllocator& m_Allocator;
        std::vector<PassNode>   m_Passes;
        std::vector<ResourceNode> m_Resources;
        std::vector<BufferNode>   m_Buffers;
        IArchiveSink*           m_ArchiveSink = nullptr;
        bool                    m_SerializeDispatch = false;

        void AllocatePhysicalResources();
        void CleanupPhysicalResources();

        // Compile sub-steps
        void CullDeadPasses();
        void SolveBarriers();
        void ComputeLifetimes();
    };
}
