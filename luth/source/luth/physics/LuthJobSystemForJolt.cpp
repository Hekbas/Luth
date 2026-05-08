#include "luthpch.h"

#include "luth/physics/LuthJobSystemForJolt.h"

#include "luth/jobs/JobSystem.h"

#include <algorithm>
#include <thread>
#include <chrono>

namespace Luth::Physics
{
    // ── LuthBarrier ──

    void LuthJobSystemForJolt::LuthBarrier::AddJob(const JobHandle& handle)
    {
        Job* job = handle.GetPtr();
        // SetBarrier returns false if the job has already completed (mBarrier was atomically set to
        // cBarrierDoneState). In that case OnJobFinished will never fire for this barrier, so we must not
        // increment the counter — it would leak as a permanent +1 wait.
        if (job->SetBarrier(this))
            Counter.Increment(1);
    }

    void LuthJobSystemForJolt::LuthBarrier::AddJobs(const JobHandle* handles, JPH::uint num)
    {
        u32 added = 0;
        for (JPH::uint i = 0; i < num; ++i)
        {
            Job* job = handles[i].GetPtr();
            if (job->SetBarrier(this))
                ++added;
        }
        if (added > 0)
            Counter.Increment(added);
    }

    void LuthJobSystemForJolt::LuthBarrier::OnJobFinished(Job* /*job*/)
    {
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
    }

    LuthJobSystemForJolt::~LuthJobSystemForJolt() = default;

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

        // JobHandle's ctor AddRef's the job. Construct it before queuing in case the job runs and completes
        // synchronously on QueueJob.
        JobHandle handle(job);

        if (numDeps == 0)
            QueueJob(job);

        return handle;
    }

    JPH::JobSystem::Barrier* LuthJobSystemForJolt::CreateBarrier()
    {
        auto barrier = std::make_unique<LuthBarrier>();
        Barrier* raw = barrier.get();
        m_Barriers.push_back(std::move(barrier));
        return raw;
    }

    void LuthJobSystemForJolt::DestroyBarrier(Barrier* barrier)
    {
        // Per Jolt's contract, the barrier is empty (all jobs finished) by the time the user calls
        // DestroyBarrier — they must have waited.
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
        Luth::JobSystem::WaitForCounter(&lb->Counter, /*targetValue*/0);
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
}
