#pragma once

#include "luth/core/types/LuthTypes.h"

#include <chrono>

namespace Luth
{
    // Global wall clock for the engine. App::Run calls Update() once per frame on the main thread;
    // any fiber that reads DeltaTime / GetTime mid-frame sees a coherent snapshot for that frame.
    class Time
    {
        using Clock = std::chrono::steady_clock;
        using TimePoint = std::chrono::time_point<Clock>;

    public:
        static void Update()
        {
            const auto now = Clock::now();

            if (!s_Initialized)
            {
                s_StartTime = now;
                s_LastFrameTime = now;
                s_Initialized = true;
                return;
            }

            if (s_FixedDelta >= 0.0f)
            {
                s_UnscaledDeltaTime = s_FixedDelta;
            }
            else
            {
                const auto deltaDuration = now - s_LastFrameTime;
                s_UnscaledDeltaTime = std::chrono::duration<f32>(deltaDuration).count();
            }
            s_DeltaTime = s_UnscaledDeltaTime * s_TimeScale;

            s_LastFrameTime = now;
            Tick();
        }

        // Time since engine start in seconds
        static f32 GetTime()
        {
            if (s_FixedDelta >= 0.0f) return static_cast<f32>(s_FrameCount) * s_FixedDelta;
            const auto duration = Clock::now() - s_StartTime;
            return std::chrono::duration<f32>(duration).count();
        }

        // Time since engine start in milliseconds
        static f32 GetTimeMS()
        {
            if (s_FixedDelta >= 0.0f) return static_cast<f32>(s_FrameCount) * s_FixedDelta * 1000.0f;
            const auto duration = Clock::now() - s_StartTime;
            return std::chrono::duration<f32, std::milli>(duration).count();
        }

        // Deterministic clock for offscreen tooling: delta AND GetTime derive from the frame count
        // instead of the wall clock. Note SetTimeScale alone cannot do this -- GetTime() reads the
        // clock directly and never consulted s_TimeScale, so the wind/gust/grain terms that sample it
        // stayed live. Two runs must land identical geometry or an A/B compares different meshes.
        // dt 0 freezes time outright (static capture); a positive dt advances it reproducibly.
        static void SetFixedDelta(f32 dt) { s_FixedDelta = dt; }
        static void ClearFixedDelta() { s_FixedDelta = -1.0f; }
        static bool IsFixedDelta() { return s_FixedDelta >= 0.0f; }

        // Scaled delta time (affected by time scale)
        static f32 DeltaTime() { return s_DeltaTime; }

        // Raw delta time (unaffected by time scale)
        static f32 UnscaledDeltaTime() { return s_UnscaledDeltaTime; }

        // Time scale (1.0 = normal, 0.5 = half speed, 2.0 = double speed)
        static f32 GetTimeScale() { return s_TimeScale; }
        static void SetTimeScale(f32 scale) { s_TimeScale = scale; }

        static u32 GetFrameCount() { return s_FrameCount; }
        static void Tick() { s_FrameCount++; }

    private:
        inline static bool s_Initialized = false;
        inline static TimePoint s_StartTime;
        inline static TimePoint s_LastFrameTime;
        inline static f32 s_TimeScale = 1.0f;
        inline static f32 s_FixedDelta = -1.0f;   // negative = live wall clock
        inline static f32 s_DeltaTime = 0.0f;
        inline static f32 s_UnscaledDeltaTime = 0.0f;
        static inline u32 s_FrameCount = 0;
    };
}
