#include "luthpch.h"

#include "luth/physics/LuthJobSystemForJolt.h"

#include "luth/core/diagnostics/Log.h"
#include "luth/jobs/JobSystem.h"

#include <algorithm>
#include <chrono>
#include <thread>

namespace Luth::Physics
{
    // ── LuthBarrier ──

    // Counter must be incremented BEFORE SetBarrier opens the gate. If we incremented after, a
    // worker running the job concurrently could swap mBarrier into cBarrierDoneState and call
    // OnJobFinished's Decrement on a counter that's still at zero — and AtomicCounter::Decrement
    // clamps at zero (its DecrementCounter has `if (old < 2) break;`). The decrement is then
    // silently swallowed and the subsequent Increment leaves the counter permanently +1, deadlocking
    // any WaitForJobs on this barrier.
    //
    // Jolt's reference JobSystemWithBarrier doesn't have this problem because its counter is a
    // semaphore — releases stack up and are never lost. AtomicCounter doesn't, so the order matters.

    void LuthJobSystemForJolt::LuthBarrier::AddJob(const JobHandle& handle)
    {
        Job* job = handle.GetPtr();

        Counter.Increment(1);                            // claim before opening the gate
        if (job->SetBarrier(this))
        {
            std::string name;
            if (m_Parent)
            {
                std::lock_guard<std::mutex> lk(m_Parent->m_NameMapMutex);
                auto it = m_Parent->m_JobNames.find(job);
                if (it != m_Parent->m_JobNames.end())
                    name = it->second;
            }
            std::lock_guard<std::mutex> lk(m_PendingMutex);
            m_Pending.push_back({ job, std::move(name) });
        }
        else
        {
            // Job already done; OnJobFinished won't fire for it. Roll back the optimistic increment.
            Counter.Decrement(1);
        }
    }

    void LuthJobSystemForJolt::LuthBarrier::AddJobs(const JobHandle* handles, JPH::uint num)
    {
        if (num == 0) return;

        Counter.Increment(num);
        u32 failed = 0;

        for (JPH::uint i = 0; i < num; ++i)
        {
            Job* job = handles[i].GetPtr();
            if (job->SetBarrier(this))
            {
                std::string name;
                if (m_Parent)
                {
                    std::lock_guard<std::mutex> lk(m_Parent->m_NameMapMutex);
                    auto it = m_Parent->m_JobNames.find(job);
                    if (it != m_Parent->m_JobNames.end())
                        name = it->second;
                }
                std::lock_guard<std::mutex> lk(m_PendingMutex);
                m_Pending.push_back({ job, std::move(name) });
            }
            else
            {
                ++failed;
            }
        }

        if (failed > 0)
            Counter.Decrement(failed);
    }

    void LuthJobSystemForJolt::LuthBarrier::OnJobFinished(Job* job)
    {
        {
            std::lock_guard<std::mutex> lk(m_PendingMutex);
            auto it = std::find_if(m_Pending.begin(), m_Pending.end(),
                [job](const PendingJob& p) { return p.job == job; });
            if (it != m_Pending.end())
                m_Pending.erase(it);
        }
        Counter.Decrement(1);
    }

    // ── LuthJobSystemForJolt ──

    LuthJobSystemForJolt::LuthJobSystemForJolt(JPH::uint maxJobs, JPH::uint maxBarriers)
    {
        m_Jobs.Init(maxJobs, maxJobs);
        m_Barriers.reserve(maxBarriers);

        // Cap our advertised concurrency below Luth's worker count so non-physics game-stage jobs always
        // have free workers to run on. GetStats() requires Luth::JobSystem::Init() to have been called;
        // adapter construction happens after engine init, so this is safe.
        const auto stats = Luth::JobSystem::GetStats();
        m_MaxConcurrency = stats.ThreadCount > 2
                         ? static_cast<int>(stats.ThreadCount) - 2
                         : 1;

        // Watchdog detects WaitForJobs calls that hang past the threshold (typically a job that never
        // fires OnJobFinished — lost in scheduling, ran without setting its barrier, or stuck inside
        // Jolt). Logs the names of the still-pending jobs the first time each stuck period is detected.
        m_WatchdogRunning.store(true, std::memory_order_release);
        m_WatchdogThread = std::thread(&LuthJobSystemForJolt::WatchdogLoop, this);
    }

    LuthJobSystemForJolt::~LuthJobSystemForJolt()
    {
        m_WatchdogRunning.store(false, std::memory_order_release);
        if (m_WatchdogThread.joinable())
            m_WatchdogThread.join();
    }

    int LuthJobSystemForJolt::GetMaxConcurrency() const
    {
        return m_MaxConcurrency;
    }

    JPH::JobSystem::JobHandle LuthJobSystemForJolt::CreateJob(
        const char* name, JPH::ColorArg color,
        const JobFunction& fn, JPH::uint32 numDeps)
    {
        // Allocate from the free list. ConstructObject returns an index or cInvalidObjectIndex when full.
        // Yield-and-retry under pressure mirrors JobSystemThreadPool's behavior; if hit often, raise maxJobs.
        JPH::uint32 index;
        for (;;)
        {
            index = m_Jobs.ConstructObject(name, color, this, fn, numDeps);
            if (index != decltype(m_Jobs)::cInvalidObjectIndex)
                break;
            std::this_thread::sleep_for(std::chrono::microseconds(100));
        }
        Job* job = &m_Jobs.Get(index);

        // Record name for the watchdog before any concurrent path (QueueJob below) can run the job and
        // call FreeJob on completion. Erase happens in FreeJob.
        {
            std::lock_guard<std::mutex> lk(m_NameMapMutex);
            m_JobNames[job] = name ? std::string(name) : std::string("<unnamed>");
        }

        // JobHandle's ctor AddRef's the job. Construct it before queuing in case the job runs and completes
        // synchronously on QueueJob.
        JobHandle handle(job);

        if (numDeps == 0)
            QueueJob(job);

        return handle;
    }

    JPH::JobSystem::Barrier* LuthJobSystemForJolt::CreateBarrier()
    {
        auto barrier  = std::make_unique<LuthBarrier>();
        barrier->m_Parent = this;
        Barrier* raw  = barrier.get();
        std::lock_guard<std::mutex> lk(m_BarriersMutex);
        m_Barriers.push_back(std::move(barrier));
        return raw;
    }

    void LuthJobSystemForJolt::DestroyBarrier(Barrier* barrier)
    {
        // Per Jolt's contract, the barrier is empty (all jobs finished) by the time the user calls
        // DestroyBarrier — they must have waited.
        std::lock_guard<std::mutex> lk(m_BarriersMutex);
        auto it = std::find_if(m_Barriers.begin(), m_Barriers.end(),
            [barrier](const std::unique_ptr<LuthBarrier>& b) { return b.get() == barrier; });
        if (it != m_Barriers.end())
            m_Barriers.erase(it);
    }

    void LuthJobSystemForJolt::WaitForJobs(Barrier* barrier)
    {
        // The whole point of this adapter: route Jolt's wait through Luth's V5 inline-execution +
        // fiber-yield path. The calling fiber will steal queued Jolt jobs from its local deque (up to depth
        // 4) and then yield, freeing the OS thread for other ready fibers.
        auto* lb = static_cast<LuthBarrier*>(barrier);

        lb->m_HasLogged.store(false, std::memory_order_release);
        lb->m_WaitStart = std::chrono::steady_clock::now();
        lb->m_IsWaiting.store(true,  std::memory_order_release);

        Luth::JobSystem::WaitForCounter(&lb->Counter, /*targetValue*/0);

        lb->m_IsWaiting.store(false, std::memory_order_release);
    }

    void LuthJobSystemForJolt::QueueJob(Job* job)
    {
        // We're storing the job for async execution — take a ref so it survives until the trampoline runs.
        // The trampoline Releases after Execute. (Jolt's JobSystem.h: "If you store the job in your own
        // data structure you need to call AddRef().")
        job->AddRef();
        Luth::JobSystem::Execute(
            TrampolineFn,
            job,
            /*counter*/nullptr,
            "JoltJob",
            Luth::JobSystem::Priority::High);
    }

    void LuthJobSystemForJolt::QueueJobs(Job** jobs, JPH::uint numJobs)
    {
        for (JPH::uint i = 0; i < numJobs; ++i)
            QueueJob(jobs[i]);
    }

    void LuthJobSystemForJolt::FreeJob(Job* job)
    {
        {
            std::lock_guard<std::mutex> lk(m_NameMapMutex);
            m_JobNames.erase(job);
        }
        m_Jobs.DestructObject(job);
    }

    void LuthJobSystemForJolt::TrampolineFn(Luth::JobSystem::JobArgs args)
    {
        auto* job = static_cast<Job*>(args.data);
        // Job::Execute() runs the job function and, at the end, atomically swaps mBarrier to
        // cBarrierDoneState and calls Barrier::OnJobFinished — which our LuthBarrier uses to decrement its
        // counter and wake any fibers waiting in WaitForJobs → WaitForCounter.
        job->Execute();
        // Drop the ref we took in QueueJob. When the last ref drops (which happens here if the user already
        // let their JobHandle expire), FreeJob is called via Job::Release().
        job->Release();
    }

    // ── Watchdog ──

    void LuthJobSystemForJolt::WatchdogLoop()
    {
        using namespace std::chrono;

        // Snapshot a stuck barrier's state under its locks, then log outside the locks. Each barrier
        // logs once per stuck-period (m_HasLogged latch); cleared on the next WaitForJobs entry.
        struct StuckSnapshot
        {
            const void*             barrierPtr;
            milliseconds            elapsed;
            std::vector<PendingJob> pending;
            u32                     counterRaw;
        };

        while (m_WatchdogRunning.load(std::memory_order_acquire))
        {
            std::this_thread::sleep_for(kWatchdogPollPeriod);

            const auto now = steady_clock::now();
            std::vector<StuckSnapshot> stuck;

            {
                std::lock_guard<std::mutex> lk(m_BarriersMutex);
                for (const auto& bp : m_Barriers)
                {
                    if (!bp->m_IsWaiting.load(std::memory_order_acquire)) continue;

                    const auto elapsed = duration_cast<milliseconds>(now - bp->m_WaitStart);
                    if (elapsed < kWatchdogThreshold) continue;

                    bool expected = false;
                    if (!bp->m_HasLogged.compare_exchange_strong(expected, true,
                                                                  std::memory_order_acq_rel))
                        continue; // Already logged for this stuck period.

                    StuckSnapshot snap;
                    snap.barrierPtr = bp.get();
                    snap.elapsed    = elapsed;
                    snap.counterRaw = bp->Counter.Value.load(std::memory_order_acquire);
                    {
                        std::lock_guard<std::mutex> lk2(bp->m_PendingMutex);
                        snap.pending = bp->m_Pending;
                    }
                    stuck.push_back(std::move(snap));
                }
            }

            for (const auto& s : stuck)
            {
                LH_CORE_WARN("[JoltAdapter Watchdog] WaitForJobs stuck on barrier {} for {} ms; "
                             "counter raw=0x{:x} (busy={}, count={}); {} job(s) outstanding:",
                             s.barrierPtr, s.elapsed.count(), s.counterRaw,
                             (s.counterRaw & 1u), (s.counterRaw >> 1),
                             s.pending.size());
                for (size_t i = 0; i < s.pending.size(); ++i)
                {
                    LH_CORE_WARN("  [{}] Job {} '{}'",
                                 i,
                                 static_cast<void*>(s.pending[i].job),
                                 s.pending[i].name.empty() ? "<unnamed>" : s.pending[i].name.c_str());
                }
            }
        }
    }
}
