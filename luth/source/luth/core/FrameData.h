#pragma once

#include "luth/core/LuthTypes.h"
#include "luth/core/AtomicCounter.h"
#include "luth/core/memory/LinearAllocator.h" 
#include <vector>

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
        // LinearAllocator LogicMemory; 

        // 2. Synchronization
        JobSystem::AtomicCounter GameReady;        // Signaled when Game Logic finishes this frame
        u64 GpuTimelineValue = 0;      // The value the GPU signals when done

        // 3. Render Resources
        // LinearAllocator RenderMemory;   // For temporary command arrays/barriers
        CommandAllocatorPool* CmdPool = nullptr;  // Thread-local command pools for this frame
        
        // List of command buffers recorded for this frame
        // std::vector<VkCommandBuffer> CommandBuffers; 
        // std::mutex CommandBufferMutex;

        void Reset()
        {
            GameReady.Value = 0;
            GameReady.WaitingListHead = nullptr;
            // LogicMemory.Reset();
            // RenderMemory.Reset();
            // CommandBuffers.clear();
        }
    };
}
