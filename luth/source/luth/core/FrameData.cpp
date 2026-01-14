#include "luthpch.h"
#include "FrameData.h"

namespace Luth
{
    void FrameContext::Init()
    {
        m_Allocator.Init();
        m_CurrentFrameIndex = 0;
    }

    void FrameContext::Shutdown()
    {
        m_Allocator.Shutdown();
    }

    void FrameContext::BeginFrame()
    {
        m_CurrentFrameIndex++;
        
        // Prepare the params for the new frame
        FrameParams& params = GetCurrentParams();
        params.FrameIndex = m_CurrentFrameIndex;
        
        // Note: We don't clear the memory here. 
        // Memory for this frame index (N) was freed when N-3 finished (RecycleFrame).
        // If we are triple buffering:
        // Frame N uses Tag 0.
        // Frame N+1 uses Tag 1.
        // Frame N+2 uses Tag 2.
        // Frame N+3 uses Tag 0. -> Must ensure Tag 0 is free!
        
        // The Poller Job calls RecycleFrame(N-3) which frees Tag 0.
        // So by the time we get here, it should be free.
        // If not, we run out of memory or grow indefinitely if we don't wait.
        // The main loop MUST wait for the GPU to be far enough ahead.
    }

    void FrameContext::RecycleFrame(u64 completedFrameIndex)
    {
        // Free all memory associated with this frame
        u32 tag = completedFrameIndex % MAX_FRAMES_IN_FLIGHT;
        m_Allocator.FreeTag(tag);
    }
}
