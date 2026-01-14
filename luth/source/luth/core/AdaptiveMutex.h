#pragma once

#include "luth/core/LuthTypes.h"
#include <atomic>
#include <thread>

namespace Luth
{
    // ===================================================================================
    // Adaptive Mutex (Fiber Aware)
    // ===================================================================================
    // A lightweight mutex that spins briefly before yielding.
    // Designed for short critical sections in a fiber-based system.
    // Does NOT block the OS thread (uses std::this_thread::yield() or Fiber::Yield()).
    
    class AdaptiveMutex
    {
    public:
        void Lock()
        {
            // 1. Try to acquire lock immediately (CAS)
            if (!m_Locked.exchange(true, std::memory_order_acquire))
                return;

            // 2. Spin loop
            for (int i = 0; i < SPIN_COUNT; ++i)
            {
                // Pause CPU pipeline to save power/reduce contention
                #if defined(_MSC_VER)
                _mm_pause();
                #elif defined(__GNUC__) || defined(__clang__)
                __builtin_ia32_pause();
                #endif

                // Check if free before trying CAS again (Test-and-Test-and-Set)
                if (!m_Locked.load(std::memory_order_relaxed))
                {
                    if (!m_Locked.exchange(true, std::memory_order_acquire))
                        return;
                }
            }

            // 3. Yield loop
            // If we failed to acquire after spinning, we yield the thread/fiber.
            // In a full fiber system, we would suspend the fiber and add it to a wait list.
            // For now, we use OS yield which is safe but less efficient than fiber suspension.
            while (m_Locked.exchange(true, std::memory_order_acquire))
            {
                std::this_thread::yield();
            }
        }

        void Unlock()
        {
            m_Locked.store(false, std::memory_order_release);
        }

        // RAII Wrapper
        struct ScopedLock
        {
            AdaptiveMutex& m_Mutex;
            ScopedLock(AdaptiveMutex& mutex) : m_Mutex(mutex) { m_Mutex.Lock(); }
            ~ScopedLock() { m_Mutex.Unlock(); }
        };

    private:
        std::atomic<bool> m_Locked = false;
        static constexpr int SPIN_COUNT = 2000; // Tunable
    };
}
