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
        m_Graph.RegisterRead(m_PassIndex, resource, ResourceState::ShaderResource);
        return resource;
    }

    ResourceHandle RenderPassBuilder::ReadTransfer(ResourceHandle resource)
    {
        m_Graph.RegisterRead(m_PassIndex, resource, ResourceState::TransferSrc);
        return resource;
    }

    ResourceHandle RenderPassBuilder::Write(ResourceHandle resource)
    {
        return m_Graph.RegisterWrite(m_PassIndex, resource, ResourceState::ColorAttachment);
    }

    ResourceHandle RenderPassBuilder::WriteTransfer(ResourceHandle resource)
    {
        return m_Graph.RegisterWrite(m_PassIndex, resource, ResourceState::TransferDst);
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

    ResourceHandle RenderGraph::ImportResource(const TextureDesc& desc, void* physicalResource, ResourceState initialState)
    {
        u32 index = (u32)m_Resources.size() + 1;
        // isTransient = false because we don't own it
        m_Resources.push_back({ desc, 0, false, initialState, initialState, physicalResource });
        return { index, 0 };
    }

    void RenderGraph::RegisterRead(u32 passIndex, ResourceHandle handle, ResourceState state)
    {
        LH_CORE_ASSERT(handle.IsValid(), "Invalid resource handle read!");
        LH_CORE_ASSERT(handle.index <= m_Resources.size(), "Resource index out of bounds!");
        m_Passes[passIndex].reads.push_back(handle);
        m_Passes[passIndex].readStates.push_back(state);
    }

    ResourceHandle RenderGraph::RegisterWrite(u32 passIndex, ResourceHandle handle, ResourceState state)
    {
        LH_CORE_ASSERT(handle.IsValid(), "Invalid resource handle write!");
        
        ResourceNode& node = m_Resources[handle.index - 1];
        node.version++;
        
        ResourceHandle newHandle = { handle.index, node.version };
        m_Passes[passIndex].writes.push_back(newHandle);
        m_Passes[passIndex].writeStates.push_back(state);
        
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
            // 1. Process Reads
            for (size_t i = 0; i < pass.reads.size(); ++i)
            {
                ResourceHandle handle = pass.reads[i];
                ResourceState targetState = pass.readStates[i];
                ResourceNode& res = m_Resources[handle.index - 1];
                
                if (res.currentState != targetState)
                {
                    Barrier barrier;
                    barrier.resource = handle;
                    barrier.before = res.currentState;
                    barrier.after = targetState;
                    
                    pass.preBarriers.push_back(barrier);
                    res.currentState = targetState;
                }
            }

            // 2. Process Writes
            for (size_t i = 0; i < pass.writes.size(); ++i)
            {
                ResourceHandle handle = pass.writes[i];
                ResourceState targetState = pass.writeStates[i];
                ResourceNode& res = m_Resources[handle.index - 1];
                
                // Override target state for Depth (if not transfer)
                if (targetState == ResourceState::ColorAttachment)
                {
                    if (res.desc.format == TextureFormat::D32_Float || 
                        res.desc.format == TextureFormat::D24_Unorm_S8_Uint)
                    {
                        targetState = ResourceState::DepthStencilAttachment;
                    }
                }

                if (res.currentState != targetState)
                {
                    Barrier barrier;
                    barrier.resource = handle;
                    barrier.before = res.currentState;
                    barrier.after = targetState;
                    
                    pass.preBarriers.push_back(barrier);
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
            pass.execute(ctx);
        }
    }
}
