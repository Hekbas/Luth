#pragma once

#include "luth/core/types/LuthMath.h"
#include "luth/core/RenderSnapshot.h"
#include "luth/jobs/AtomicCounter.h"
#include "luth/jobs/SpinLock.h"
#include "luth/memory/LinearAllocator.h"
#include <vector>
#include <array>
#include <vulkan/vulkan.h>

namespace Luth
{
    class CommandAllocatorPool;

    static constexpr u32 MAX_FRAMES_IN_FLIGHT = 3;

    // ── Frame Params — read-only data packet for the frame ──
    // Written by Game(N). Read by Render(N-1). Immutable after GameReady signals.

    struct FrameParams
    {
        // Time
        f32 DeltaTime = 0.0f;
        f32 TotalTime = 0.0f;
        u64 FrameNumber = 0;

        // Camera
        Mat4 ViewMatrix{1.0f};
        Mat4 ProjectionMatrix{1.0f};
        Vec3 CameraPosition{0.0f};
        Vec3 CameraForward{0.0f, 0.0f, -1.0f};

        // Viewport
        u32 ViewportWidth = 0;
        u32 ViewportHeight = 0;
    };

    // ── Frame Context — one per in-flight frame (triple buffered) ──

    struct FrameContext
    {
        // ---- Data Packet ----
        FrameParams Params;
        
        // ---- Synchronization ----
        JobSystem::AtomicCounter GameReady;     // Signaled when Game Logic finishes
        JobSystem::AtomicCounter RenderReady;   // Signaled when Render Recording finishes
        u64 GpuTimelineValue = 0;               // Timeline value GPU signals when done
        bool GpuFinished = false;               // Set by PollerJob when GPU is done with this frame

        // ---- Memory ----
        Memory::LinearAllocator LogicMemory;    // Game-thread allocations
        Memory::LinearAllocator RenderMemory;   // Render-thread allocations (barriers, cmd arrays)

        // ---- Render Resources ----
        CommandAllocatorPool* CmdPool = nullptr;

        // V6: Overflow allocator tier
        // If GPU(N-2) hasn't finished when frame N starts, we can't reset the primary
        // allocators. Instead, this frame uses overflow memory. Pages are tagged with
        // the frame index and reclaimed when GPU eventually finishes.
        bool UsingOverflow = false;

        // Secondary command buffers collected from parallel recording
        std::vector<VkCommandBuffer> CommandBuffers;
        SpinLock CommandBufferLock; // Replaces std::mutex (V1 compliant)

        // ---- Render Snapshot ----
        // Captured at end of game stage (Game N), read by render stage of frame N+1 (Render N-1
        // from N+1's perspective). Spans into LogicMemory; reset alongside it. See RenderSnapshot.h.
        RenderSnapshot Snapshot;

        void AddCommandBuffer(VkCommandBuffer cmd)
        {
            SpinLockGuard lock(CommandBufferLock);
            CommandBuffers.push_back(cmd);
        }

        FrameContext()
            : LogicMemory(10 * 1024 * 1024),   // 10MB per frame for logic
              RenderMemory(10 * 1024 * 1024)    // 10MB per frame for render
        {
            CommandBuffers.reserve(16);
        }

        void Reset()
        {
            Params = {};
            GameReady.Value = 0;
            GameReady.WaitingListHead = nullptr;
            // Defense-in-depth: a stuck Lock would deadlock WaitForCounter
            // forever on this counter; Reset is the only safe clear point.
            GameReady.Lock.clear(std::memory_order_release);
            RenderReady.Value = 0;
            RenderReady.WaitingListHead = nullptr;
            RenderReady.Lock.clear(std::memory_order_release);
            GpuFinished = false;
            UsingOverflow = false;

            // Snapshot spans point into LogicMemory; clear them BEFORE resetting the allocator
            // so we never carry dangling pointers across frames.
            Snapshot.Clear();

            LogicMemory.Reset();
            RenderMemory.Reset();

            {
                SpinLockGuard lock(CommandBufferLock);
                CommandBuffers.clear();
            }
        }
    };

    // ── Frame Data — triple-buffered ring ──
    // Owned exclusively by App. Passed to systems by reference.

    class FrameData
    {
    public:
        void Init()
        {
            m_FrameIndex = 0;
            m_RenderFrameIndex = 0;
            for (auto& f : m_Frames) f.Reset();
        }

        void Shutdown() {}

        // Current frame (Game N writes here)
        FrameContext& Current()  { return m_Frames[m_FrameIndex % MAX_FRAMES_IN_FLIGHT]; }

        // Previous frame (Render N-1 reads here in steady state)
        FrameContext& Previous() { return m_Frames[(m_FrameIndex - 1) % MAX_FRAMES_IN_FLIGHT]; }

        // Two frames ago (GPU N-2, check for completion)
        FrameContext& GPU()      { return m_Frames[(m_FrameIndex - 2) % MAX_FRAMES_IN_FLIGHT]; }

        // Slot the render stage targets this iteration — set by App::Run
        // before dispatching RenderStageFn. Decoupled from m_FrameIndex so
        // game and render can write/read different slots concurrently.
        FrameContext& RenderFrame() { return m_Frames[m_RenderFrameIndex % MAX_FRAMES_IN_FLIGHT]; }
        void SetRenderFrameIndex(u64 index) { m_RenderFrameIndex = index; }
        u64 GetRenderFrameIndex() const { return m_RenderFrameIndex; }

        // Access by absolute index
        FrameContext& GetFrame(u64 index) { return m_Frames[index % MAX_FRAMES_IN_FLIGHT]; }

        u64 GetFrameIndex() const { return m_FrameIndex; }

        void Advance() { m_FrameIndex++; }

    private:
        std::array<FrameContext, MAX_FRAMES_IN_FLIGHT> m_Frames;
        u64 m_FrameIndex = 0;
        u64 m_RenderFrameIndex = 0;
    };
}
