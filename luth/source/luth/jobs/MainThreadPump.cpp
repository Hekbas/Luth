#include "luthpch.h"
#include "luth/jobs/MainThreadPump.h"

#include "luth/core/diagnostics/Log.h"
#include "luth/memory/MemoryTracker.h"

#include <atomic>
#include <exception>
#include <mutex>
#include <queue>
#include <thread>

namespace Luth
{
    namespace {
        std::queue<MainThreadPump::Callback> s_Queue;
        std::mutex                           s_Lock;
        // Advisory counter for PendingCount diagnostics. The queue itself is mutex-protected,
        // so the relaxed atomic updates around the lock carry no synchronization burden.
        std::atomic<u32>                     s_Pending{ 0 };
    }

    void MainThreadPump::Post(Callback cb)
    {
        if (!cb) return;

        {
            std::lock_guard<std::mutex> guard(s_Lock);
            s_Queue.emplace(std::move(cb));
        }
        s_Pending.fetch_add(1, std::memory_order_relaxed);

        // Allocation accounting outside the lock; matches EventBus precedent. Tracks the std::function
        // wrapper size; the lambda's heap capture (if any) is caught by Tracy's global new hook.
        Memory::MemoryTracker::RecordAlloc(Memory::Category::Editor, sizeof(Callback));
    }

    void MainThreadPump::Drain()
    {
    #ifdef LUTH_BUILD_DEBUG
        static const std::thread::id s_DrainThread = std::this_thread::get_id();
        LH_CORE_ASSERT(std::this_thread::get_id() == s_DrainThread,
            "MainThreadPump::Drain called from inconsistent thread");
    #endif

        // Swap the live queue into a local under the lock, then drop it. Callbacks that re-Post during
        // dispatch land on the next frame's queue, not this drain (same reentrancy contract as EventBus).
        std::queue<Callback> processing;
        {
            std::lock_guard<std::mutex> guard(s_Lock);
            processing.swap(s_Queue);
        }

        while (!processing.empty()) {
            Callback& cb = processing.front();
            try {
                cb();
            } catch (const std::exception& e) {
                LH_LOG(Jobs, error, "MainThreadPump callback threw: {}", e.what());
            } catch (...) {
                LH_LOG(Jobs, error, "MainThreadPump callback threw a non-std exception");
            }

            Memory::MemoryTracker::RecordFree(Memory::Category::Editor, sizeof(Callback));
            s_Pending.fetch_sub(1, std::memory_order_relaxed);
            processing.pop();
        }
    }

    u32 MainThreadPump::PendingCount()
    {
        return s_Pending.load(std::memory_order_relaxed);
    }
}
