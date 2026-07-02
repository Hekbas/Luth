#pragma once

#include "luth/core/types/LuthTypes.h"
#include <atomic>
#include <vector>

namespace Luth
{
    // Per-worker Chase-Lev work-stealing deque for normal-priority jobs (Chase & Lev SPAA 2005,
    // Lê et al. PPoPP 2013). The owner pushes and pops at the bottom in LIFO order for cache
    // locality; thieves steal from the top in FIFO order for load balancing. Capacity doubles
    // on growth.

    template<typename T>
    class WorkStealingDeque
    {
    public:
        explicit WorkStealingDeque(i32 initialCapacity = 1024)
        {
            m_Buffer.store(new CircularBuffer(initialCapacity), std::memory_order_relaxed);
            m_Top.store(0, std::memory_order_relaxed);
            m_Bottom.store(0, std::memory_order_relaxed);
        }

        ~WorkStealingDeque()
        {
            CircularBuffer* buf = m_Buffer.load(std::memory_order_relaxed);
            delete buf;
            // Old buffers from resize are intentionally leaked; with frame-lifetime allocators this is moot.
            // A production engine would track them in a deferred deletion list.
        }

        // Owner only: push to bottom (LIFO)
        void Push(const T& item)
        {
            i64 bottom = m_Bottom.load(std::memory_order_relaxed);
            i64 top = m_Top.load(std::memory_order_acquire);
            CircularBuffer* buf = m_Buffer.load(std::memory_order_relaxed);

            if (bottom - top >= buf->Capacity() - 1)
            {
                buf = buf->Grow(top, bottom);
                m_Buffer.store(buf, std::memory_order_relaxed);
            }

            buf->Set(bottom, item);

            // Release fence: the item must be visible before bottom is published
            std::atomic_thread_fence(std::memory_order_release);
            m_Bottom.store(bottom + 1, std::memory_order_relaxed);
        }

        // Owner only: pop from bottom (LIFO)
        bool TryPop(T& out)
        {
            i64 bottom = m_Bottom.load(std::memory_order_relaxed) - 1;
            CircularBuffer* buf = m_Buffer.load(std::memory_order_relaxed);
            m_Bottom.store(bottom, std::memory_order_relaxed);

            // Full fence: the bottom write must be visible before top is read
            std::atomic_thread_fence(std::memory_order_seq_cst);

            i64 top = m_Top.load(std::memory_order_relaxed);

            if (top <= bottom)
            {
                // Non-empty
                out = buf->Get(bottom);

                if (top == bottom)
                {
                    // Last element: race with a thief. compare_exchange_strong overwrites `top` on failure,
                    // so capture the original for the Chase-Lev bottom restore (Bottom = origTop + 1, both paths).
                    const i64 origTop = top;
                    const bool won = m_Top.compare_exchange_strong(top, top + 1,
                        std::memory_order_seq_cst, std::memory_order_relaxed);
                    m_Bottom.store(origTop + 1, std::memory_order_relaxed);
                    if (!won) return false;
                }
                return true;
            }
            else
            {
                // Empty
                m_Bottom.store(top, std::memory_order_relaxed);
                return false;
            }
        }

        // Any thread: steal from top (FIFO)
        bool TrySteal(T& out)
        {
            i64 top = m_Top.load(std::memory_order_acquire);

            // Fence for a consistent bottom read relative to top
            std::atomic_thread_fence(std::memory_order_seq_cst);

            i64 bottom = m_Bottom.load(std::memory_order_acquire);

            if (top < bottom)
            {
                // Non-empty
                CircularBuffer* buf = m_Buffer.load(std::memory_order_consume);
                out = buf->Get(top);

                if (!m_Top.compare_exchange_strong(top, top + 1,
                    std::memory_order_seq_cst, std::memory_order_relaxed))
                {
                    // Lost race to owner or another thief
                    return false;
                }
                return true;
            }

            return false; // Empty
        }

        i64 Size() const
        {
            i64 bottom = m_Bottom.load(std::memory_order_relaxed);
            i64 top = m_Top.load(std::memory_order_relaxed);
            return bottom - top;
        }

        bool IsEmpty() const { return Size() <= 0; }

    private:
        // Growable circular buffer
        struct CircularBuffer
        {
            explicit CircularBuffer(i64 capacity)
                : m_Capacity(capacity), m_Mask(capacity - 1)
            {
                m_Data = new T[capacity];
            }

            ~CircularBuffer() { delete[] m_Data; }

            i64 Capacity() const { return m_Capacity; }

            void Set(i64 index, const T& item) { m_Data[index & m_Mask] = item; }
            T Get(i64 index) const { return m_Data[index & m_Mask]; }

            CircularBuffer* Grow(i64 top, i64 bottom)
            {
                CircularBuffer* newBuf = new CircularBuffer(m_Capacity * 2);
                for (i64 i = top; i < bottom; ++i)
                    newBuf->Set(i, Get(i));
                return newBuf;
                // Old buffer is intentionally not deleted here (concurrent readers may still access it).
            }

        private:
            T* m_Data;
            i64 m_Capacity;
            i64 m_Mask;
        };

        alignas(64) std::atomic<i64> m_Top;
        alignas(64) std::atomic<i64> m_Bottom;
        std::atomic<CircularBuffer*> m_Buffer;
    };
}
