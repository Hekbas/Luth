#pragma once

#include "luth/core/LuthTypes.h"

#include <chrono>

namespace Luth
{
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

            const auto deltaDuration = now - s_LastFrameTime;
            s_UnscaledDeltaTime = std::chrono::duration<f32>(deltaDuration).count();
            s_DeltaTime = s_UnscaledDeltaTime * s_TimeScale;

            s_LastFrameTime = now;
        }

        // Time since engine start in seconds
        static f32 GetTime()
        {
            const auto duration = Clock::now() - s_StartTime;
            return std::chrono::duration<f32>(duration).count();
        }

        // Time since engine start in milliseconds
        static f32 GetTimeMS()
        {
            const auto duration = Clock::now() - s_StartTime;
            return std::chrono::duration<f32, std::milli>(duration).count();
        }

        // Scaled delta time (affected by time scale)
        static f32 DeltaTime() { return s_DeltaTime; }

        // Raw delta time (unaffected by time scale)
        static f32 UnscaledDeltaTime() { return s_UnscaledDeltaTime; }

        // Time scale (1.0 = normal, 0.5 = half speed, 2.0 = double speed)
        static f32 GetTimeScale() { return s_TimeScale; }
        static void SetTimeScale(f32 scale) { s_TimeScale = scale; }

    private:
        inline static bool s_Initialized = false;
        inline static TimePoint s_StartTime;
        inline static TimePoint s_LastFrameTime;
        inline static f32 s_TimeScale = 1.0f;
        inline static f32 s_DeltaTime = 0.0f;
        inline static f32 s_UnscaledDeltaTime = 0.0f;
    };
}
