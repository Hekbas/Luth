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
        m_Passes.reserve(64);
        m_Resources.reserve(256);
    }

    ResourceHandle RenderGraph::RegisterResource(const TextureDesc& desc)
    {
        u32 index = (u32)m_Resources.size() + 1; // 1-based index
        m_Resources.push_back({ desc, 0, true, ResourceState::Undefined, ResourceState::Undefined });
        return { index, 0 };
    }

    void RenderGraph::RegisterRead(u32 passIndex, ResourceHandle handle)
    {
        LH_CORE_ASSERT(handle.IsValid(), "Invalid resource handle read!");
        LH_CORE_ASSERT(handle.index <= m_Resources.size(), "Resource index out of bounds!");
        m_Passes[passIndex].reads.push_back(handle);
    }

    ResourceHandle RenderGraph::RegisterWrite(u32 passIndex, ResourceHandle handle)
    {
        LH_CORE_ASSERT(handle.IsValid(), "Invalid resource handle write!");
        
        ResourceNode& node = m_Resources[handle.index - 1];
        node.version++;
        
        ResourceHandle newHandle = { handle.index, node.version };
        m_Passes[passIndex].writes.push_back(newHandle);
        
        return newHandle;
    }

    void RenderGraph::Compile()
    {
        LH_PROFILE_FUNCTION();

        // Reset resource states for simulation
        for (auto& res : m_Resources)
        {
            res.currentState = res.initialState;
        }

        // Iterate passes to inject barriers
        for (auto& pass : m_Passes)
        {
            // 1. Process Reads (Transition to ShaderResource)
            for (const auto& handle : pass.reads)
            {
                ResourceNode& res = m_Resources[handle.index - 1];
                
                // If current state is NOT ShaderResource, we need a barrier
                // Optimization: If it's DepthStencilReadOnly, we might not need a barrier if we just read depth
                if (res.currentState != ResourceState::ShaderResource)
                {
                    Barrier barrier;
                    barrier.resource = handle;
                    barrier.before = res.currentState;
                    barrier.after = ResourceState::ShaderResource;
                    
                    pass.preBarriers.push_back(barrier);
                    
                    // Update state
                    res.currentState = ResourceState::ShaderResource;
                }
            }

            // 2. Process Writes (Transition to ColorAttachment or DepthAttachment)
            for (const auto& handle : pass.writes)
            {
                ResourceNode& res = m_Resources[handle.index - 1];
                
                // Determine target state based on format (simple heuristic for now)
                // TODO: Pass builder should specify access type (RenderTarget vs UAV)
                ResourceState targetState = ResourceState::ColorAttachment;
                if (res.desc.format == TextureFormat::D32_Float || 
                    res.desc.format == TextureFormat::D24_Unorm_S8_Uint)
                {
                    targetState = ResourceState::DepthStencilAttachment;
                }

                if (res.currentState != targetState)
                {
                    Barrier barrier;
                    barrier.resource = handle;
                    barrier.before = res.currentState;
                    barrier.after = targetState;
                    
                    pass.preBarriers.push_back(barrier);
                    
                    // Update state
                    res.currentState = targetState;
                }
            }
        }
    }

    void RenderGraph::Execute()
    {
        LH_PROFILE_FUNCTION();

        RenderPassContext ctx; 
        
        for (const auto& pass : m_Passes)
        {
            LH_PROFILE_SCOPE(pass.name.c_str());
            
            // Log barriers for debugging (since we don't have Vulkan backend yet)
            if (!pass.preBarriers.empty())
            {
                // LH_CORE_INFO("Pass {0} Barriers:", pass.name);
                for (const auto& b : pass.preBarriers)
                {
                    // LH_CORE_INFO("  Res {0}: {1} -> {2}", b.resource.index, (int)b.before, (int)b.after);
                }
            }
            
            pass.execute(ctx);
        }
    }
}
