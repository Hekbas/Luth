#pragma once

#include "luth/core/types/LuthTypes.h"
#include <chrono>

namespace Luth
{
    // Lightweight scoped stopwatch. The constructor starts the clock; Elapsed / ElapsedMillis sample on
    // demand. Independent of Time's global clock so callers can measure sub-frame intervals without
    // coupling to the frame-loop tick.
    class Timer
    {
    public:
        Timer()
        {
            Reset();
        }

        void Reset()
        {
            m_Start = std::chrono::high_resolution_clock::now();
        }

        f32 Elapsed()
        {
            return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::high_resolution_clock::now() - m_Start).count() * 0.001f * 0.001f * 0.001f;
        }

        f32 ElapsedMillis()
        {
            return Elapsed() * 1000.0f;
        }

    private:
        std::chrono::time_point<std::chrono::high_resolution_clock> m_Start;
    };
}
