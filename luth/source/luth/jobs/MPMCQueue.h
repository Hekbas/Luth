#pragma once

#include "luth/core/types/LuthTypes.h"
#include <atomic>
#include <array>
#include <new> // std::hardware_destructive_interference_size

#ifdef _WIN32
#include <windows.h> // WaitOnAddress, WakeByAddressSingle
#endif

namespace Luth
{
    // ===================================================================================
    // MPMCQueue — Lock-Free Bounded Multi-Producer Multi-Consumer Queue
    // ===================================================================================
    // Used for the Global High-Priority Job Queue.
    // V4 Compliant: All insertions pair with WakeByAddressSingle to prevent lost wakeups.
    //
    // Based on Dmitry Vyukov's bounded MPMC queue.
    // Capacity MUST be a power of 2.

    template<typename T, u32 Capacity>
    class MPMCQueue
    {
        static_assert((Capacity & (Capacity - 1)) == 0, "MPMCQueue capacity must be a power of 2");
        static_assert(Capacity >= 2, "MPMCQueue capacity must be at least 2");

    public:
        MPMCQueue()
        {
            for (u32 i = 0; i < Capacity; ++i)
                m_Cells[i].Sequence.store(i, std::memory_order_relaxed);
            m_EnqueuePos.store(0, std::memory_order_relaxed);
            m_DequeuePos.store(0, std::memory_order_relaxed);
            m_Generation.store(0, std::memory_order_relaxed);
        }

        // Returns true if the item was enqueued.
        // Returns false if the queue is full.
        bool TryPush(const T& item)
        {
            Cell* cell;
            u32 pos = m_EnqueuePos.load(std::memory_order_relaxed);

            while (true)
            {
                cell = &m_Cells[pos & (Capacity - 1)];
                u32 seq = cell->Sequence.load(std::memory_order_acquire);
                i32 diff = (i32)seq - (i32)pos;

                if (diff == 0)
                {
                    // Slot is available — try to claim it
                    if (m_EnqueuePos.compare_exchange_weak(pos, pos + 1, std::memory_order_relaxed))
                        break;
                }
                else if (diff < 0)
                {
                    // Queue is full
                    return false;
                }
                else
                {
                    // Another producer claimed this slot, reload
                    pos = m_EnqueuePos.load(std::memory_order_relaxed);
                }
            }

            cell->Data = item;
            cell->Sequence.store(pos + 1, std::memory_order_release);

            // V4: Signal that new work is available
            m_Generation.fetch_add(1, std::memory_order_release);
#ifdef _WIN32
            WakeByAddressSingle(&m_Generation);
#endif

            return true;
        }

        // Returns true if an item was dequeued into `out`.
        // Returns false if the queue is empty.
        bool TryPop(T& out)
        {
            Cell* cell;
            u32 pos = m_DequeuePos.load(std::memory_order_relaxed);

            while (true)
            {
                cell = &m_Cells[pos & (Capacity - 1)];
                u32 seq = cell->Sequence.load(std::memory_order_acquire);
                i32 diff = (i32)seq - (i32)(pos + 1);

                if (diff == 0)
                {
                    // Data is ready — try to claim it
                    if (m_DequeuePos.compare_exchange_weak(pos, pos + 1, std::memory_order_relaxed))
                        break;
                }
                else if (diff < 0)
                {
                    // Queue is empty
                    return false;
                }
                else
                {
                    // Another consumer claimed this slot, reload
                    pos = m_DequeuePos.load(std::memory_order_relaxed);
                }
            }

            out = cell->Data;
            cell->Sequence.store(pos + Capacity, std::memory_order_release);
            return true;
        }

        // V4: Generation counter for WaitOnAddress protocol.
        // Workers load this value, check queues, then WaitOnAddress if empty.
        u32 GetGeneration() const { return m_Generation.load(std::memory_order_acquire); }
        std::atomic<u32>* GetGenerationPtr() { return &m_Generation; }

        bool IsEmpty() const
        {
            u32 enq = m_EnqueuePos.load(std::memory_order_relaxed);
            u32 deq = m_DequeuePos.load(std::memory_order_relaxed);
            return enq == deq;
        }

        u32 GetSize() const
        {
            u32 enq = m_EnqueuePos.load(std::memory_order_relaxed);
            u32 deq = m_DequeuePos.load(std::memory_order_relaxed);
            return enq - deq;
        }

    private:
        struct Cell
        {
            std::atomic<u32> Sequence;
            T Data;
        };

        // Pad to avoid false sharing between enqueue and dequeue positions
        alignas(64) std::atomic<u32> m_EnqueuePos;
        alignas(64) std::atomic<u32> m_DequeuePos;
        alignas(64) std::atomic<u32> m_Generation; // V4: Wakeup signal counter
        alignas(64) std::array<Cell, Capacity> m_Cells;
    };
}
