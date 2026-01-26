#pragma once

#include "luth/core/LuthTypes.h"
#include "luth/core/AtomicCounter.h"
#include "luth/core/memory/LinearAllocator.h" 
#include <vector>
#include <array>
#include <mutex>
#include <vulkan/vulkan.h>

namespace Luth
{
    // Forward declarations
    class CommandAllocatorPool;

    // ===================================================================================
    // Frame Context (Triple Buffered)
    // ===================================================================================

    struct FrameParams
    {
        // Global Matrices
        // Camera Data
        // Time
        f32 DeltaTime;
        f32 TotalTime;
        u64 FrameNumber;
    };

    struct FrameContext
    {
        static constexpr u32 MAX_FRAMES_IN_FLIGHT = 3;

        // 1. Data Packet (Written by Game, Read-Only by Render)
        FrameParams Params;
        
        // Memory for Game Logic (cleared after GPU finishes N-2)
        Memory::LinearAllocator LogicMemory; 

        // 2. Synchronization
        JobSystem::AtomicCounter GameReady;        // Signaled when Game Logic finishes this frame
        u64 GpuTimelineValue = 0;      // The value the GPU signals when done

        // 3. Render Resources
        Memory::LinearAllocator RenderMemory;   // For temporary command arrays/barriers
        CommandAllocatorPool* CmdPool = nullptr;  // Thread-local command pools for this frame
        
        // List of command buffers recorded for this frame (Secondary Buffers)
        std::vector<VkCommandBuffer> CommandBuffers; 
        std::mutex CommandBufferMutex;

        FrameContext() 
            : LogicMemory(10 * 1024 * 1024), // 10MB per frame for logic
              RenderMemory(10 * 1024 * 1024) // 10MB per frame for render commands
        {}

        void Reset()
        {
            GameReady.Value = 0;
            GameReady.WaitingListHead = nullptr;
            LogicMemory.Reset();
            RenderMemory.Reset();
            
            std::lock_guard<std::mutex> lock(CommandBufferMutex);
            CommandBuffers.clear();
        }
    };

    // Container for the ring buffer
    class FrameData
    {
    public:
        void Init()
        {
            m_FrameIndex = 0;
            for(auto& f : m_Frames) f.Reset();
        }

        void Shutdown()
        {
            // Cleanup if needed
        }

        FrameContext& GetFrame(u64 index)
        {
            return m_Frames[index % FrameContext::MAX_FRAMES_IN_FLIGHT];
        }

        FrameContext& GetCurrentFrame()
        {
            return GetFrame(m_FrameIndex);
        }
        
        FrameContext& GetPreviousFrame()
        {
            return GetFrame(m_FrameIndex - 1);
        }

        u64 GetCurrentFrameIndex() const { return m_FrameIndex; }
        
        void Advance()
        {
            m_FrameIndex++;
        }

    private:
        std::array<FrameContext, FrameContext::MAX_FRAMES_IN_FLIGHT> m_Frames;
        u64 m_FrameIndex = 0;
    };
}
