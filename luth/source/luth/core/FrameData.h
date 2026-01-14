#pragma once

#include "luth/core/LuthTypes.h"
#include "luth/core/memory/TaggedPageAllocator.h"
#include <array>

namespace Luth
{
    // ===================================================================================
    // Frame Parameters (Immutable Input for Render Stage)
    // ===================================================================================
    // This struct contains EVERYTHING the renderer needs to know about the game state
    // for a specific frame. It is written by the Game Logic (Frame N) and read by
    // the Renderer (Frame N-1).
    struct FrameParams
    {
        u64 FrameIndex = 0;
        f32 Time = 0.0f;
        f32 DeltaTime = 0.0f;

        // Viewport
        u32 ViewportWidth = 0;
        u32 ViewportHeight = 0;

        // Camera Data (Matrices)
        // TODO: Add Camera struct

        // Input State (if needed by renderer, e.g. for debug camera)
        
        // Scene Data
        // Pointers to data allocated in the TaggedPageAllocator for this frame.
        // e.g. Array of RenderObjects, Lights, etc.
        void* RenderObjects = nullptr; 
        u32 RenderObjectCount = 0;
    };

    // ===================================================================================
    // Frame Context (Triple Buffered)
    // ===================================================================================
    class FrameContext
    {
    public:
        static constexpr u32 MAX_FRAMES_IN_FLIGHT = 3;

        FrameContext() = default;

        void Init();
        void Shutdown();

        // Called at the start of the Game Logic frame (N)
        void BeginFrame();

        // Called when the GPU finishes Frame N-2
        void RecycleFrame(u64 completedFrameIndex);

        // Accessors
        FrameParams& GetCurrentParams() { return m_FrameParams[m_CurrentFrameIndex % MAX_FRAMES_IN_FLIGHT]; }
        const FrameParams& GetRenderParams() const { return m_FrameParams[(m_CurrentFrameIndex - 1) % MAX_FRAMES_IN_FLIGHT]; }
        
        Memory::TaggedPageAllocator& GetAllocator() { return m_Allocator; }
        
        u64 GetCurrentFrameIndex() const { return m_CurrentFrameIndex; }

    private:
        u64 m_CurrentFrameIndex = 0;
        
        std::array<FrameParams, MAX_FRAMES_IN_FLIGHT> m_FrameParams;
        
        // The global allocator. 
        // We use tags to separate frames: Tag = FrameIndex % MAX_FRAMES_IN_FLIGHT
        Memory::TaggedPageAllocator m_Allocator;
    };
}
