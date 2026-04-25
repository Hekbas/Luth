#pragma once

#include <atomic>
#include <emmintrin.h> // _mm_pause

namespace Luth
{
    // SpinLock (V1 — see arch/version-glossary.md).
    // Pure spin-lock for micro-critical sections (< 100 cycles).
    // NEVER yields, NEVER sleeps. If your critical section is long enough
    // to need either, restructure it as a lock-free atomic state or a job chain.
    //
    // Contract: Never call Fiber::Yield() while holding this lock.

    class SpinLock
    {
    public:
        void Lock() noexcept
        {
            // Fast path: try to acquire immediately
            if (!m_Flag.test_and_set(std::memory_order_acquire))
                return;

            // Slow path: spin with backoff
            while (true)
            {
                // Spin without writing (reduces cache-line bouncing)
                // test() is a read-only check — doesn't invalidate other cores' caches
                while (m_Flag.test(std::memory_order_relaxed))
                {
                    _mm_pause(); // Intel PAUSE — reduces pipeline stalls during spin
                }

                // Try to acquire
                if (!m_Flag.test_and_set(std::memory_order_acquire))
                    return;
            }
        }

        void Unlock() noexcept
        {
            m_Flag.clear(std::memory_order_release);
        }

        bool TryLock() noexcept
        {
            return !m_Flag.test_and_set(std::memory_order_acquire);
        }

    private:
        std::atomic_flag m_Flag = ATOMIC_FLAG_INIT;
    };

    // RAII guard
    class SpinLockGuard
    {
    public:
        explicit SpinLockGuard(SpinLock& lock) : m_Lock(lock) { m_Lock.Lock(); }
        ~SpinLockGuard() { m_Lock.Unlock(); }

        SpinLockGuard(const SpinLockGuard&) = delete;
        SpinLockGuard& operator=(const SpinLockGuard&) = delete;

    private:
        SpinLock& m_Lock;
    };
}
