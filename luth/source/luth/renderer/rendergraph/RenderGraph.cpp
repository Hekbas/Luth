#include "luthpch.h"
#include "luth/renderer/rendergraph/RenderGraph.h"
#include "luth/core/Log.h"
#include "luth/core/Profiler.h"

namespace Luth::RG
{
    // ===================================================================================
    // RenderPassBuilder Implementation
    // ===================================================================================

    ResourceHandle RenderPassBuilder::Read(ResourceHandle resource)
    {
        m_Graph.RegisterRead(m_PassIndex, resource);
        return resource;
    }

    ResourceHandle RenderPassBuilder::Write(ResourceHandle resource)
    {
        return m_Graph.RegisterWrite(m_PassIndex, resource);
    }

    ResourceHandle RenderPassBuilder::CreateTexture(const TextureDesc& desc)
    {
        return m_Graph.RegisterResource(desc);
    }

    // ===================================================================================
    // RenderGraph Implementation
    // ===================================================================================

    RenderGraph::RenderGraph(LinearAllocator& allocator)
        : m_Allocator(allocator)
    {
        // Reserve some space to avoid reallocations during setup
        m_Passes.reserve(64);
        m_Resources.reserve(256);
    }

    ResourceHandle RenderGraph::RegisterResource(const TextureDesc& desc)
    {
        u32 index = (u32)m_Resources.size() + 1; // 1-based index
        m_Resources.push_back({ desc, 0, true });
        return { index, 0 };
    }

    void RenderGraph::RegisterRead(u32 passIndex, ResourceHandle handle)
    {
        LH_CORE_ASSERT(handle.IsValid(), "Invalid resource handle read!");
        LH_CORE_ASSERT(handle.index <= m_Resources.size(), "Resource index out of bounds!");

        // Verify version? 
        // In a strict graph, we might check if handle.version == m_Resources[handle.index-1].version
        
        m_Passes[passIndex].reads.push_back(handle);
    }

    ResourceHandle RenderGraph::RegisterWrite(u32 passIndex, ResourceHandle handle)
    {
        LH_CORE_ASSERT(handle.IsValid(), "Invalid resource handle write!");
        
        // Increment version
        ResourceNode& node = m_Resources[handle.index - 1];
        node.version++;
        
        ResourceHandle newHandle = { handle.index, node.version };
        m_Passes[passIndex].writes.push_back(newHandle);
        
        return newHandle;
    }

    void RenderGraph::Compile()
    {
        LH_PROFILE_FUNCTION();

        // 1. Cull unused passes (Ref counting)
        // For now, we assume all passes are active (immediate mode style)
        // TODO: Implement culling starting from "Backbuffer" or "External" writes.

        // 2. Calculate Barriers
        // Iterate passes linearly.
        // For each resource, track its "Current State" (Layout, Access).
        // If a pass needs Read access but current state is Write, insert barrier.
        
        // This requires a "ResourceState" tracker map.
        // std::unordered_map<u32, ResourceState> resourceStates;
        
        // For the prototype, we just log the graph structure
        /*
        LH_CORE_INFO("--- Render Graph Compile ---");
        for (const auto& pass : m_Passes)
        {
            LH_CORE_INFO("Pass: {0}", pass.name);
            for (auto r : pass.reads) LH_CORE_INFO("  Read: Res {0} v{1}", r.index, r.version);
            for (auto w : pass.writes) LH_CORE_INFO("  Write: Res {0} v{1}", r.index, r.version);
        }
        */
    }

    void RenderGraph::Execute()
    {
        LH_PROFILE_FUNCTION();

        // 3. Execute Passes
        // We can dispatch these to the JobSystem!
        // However, Vulkan Command Buffer recording must be careful about thread safety 
        // if using a single pool, or we allocate a pool per thread.
        
        // For Phase 1 of RenderGraph, let's execute serially to ensure correctness,
        // then parallelize.
        
        RenderPassContext ctx; // Dummy context for now
        
        for (const auto& pass : m_Passes)
        {
            LH_PROFILE_SCOPE(pass.name.c_str());
            
            // TODO: Insert Barriers here (vkCmdPipelineBarrier)
            
            // Execute the lambda
            pass.execute(ctx);
        }
    }
}
