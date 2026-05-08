#pragma once

#include <Jolt/Jolt.h>
#include <Jolt/Core/JobSystem.h>
#include <Jolt/Core/FixedSizeFreeList.h>

#include "luth/jobs/JobSystem.h"
#include "luth/jobs/AtomicCounter.h"

#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace Luth::Physics
{
    // JPH::JobSystem adapter that runs Jolt's parallel work through Luth's fiber-based scheduler.
    // WaitForJobs routes through WaitForCounter so barrier waits exercise the V5 inline-execution +
    // fiber-yield path — the whole reason for picking direct JPH::JobSystem over the built-in
    // semaphore-backed JobSystemWithBarrier.
    class LuthJobSystemForJolt final : public JPH::JobSystem
    {
    public:
        // maxJobs caps concurrent Jolt jobs in flight; maxBarriers is a soft hint for vector reserve
        // (barriers can grow beyond it).
        explicit LuthJobSystemForJolt(JPH::uint maxJobs = 2048, JPH::uint maxBarriers = 8);
        ~LuthJobSystemForJolt() override;

        int       GetMaxConcurrency() const override;
        JobHandle CreateJob(const char* name, JPH::ColorArg color,
                            const JobFunction& fn, JPH::uint32 numDeps = 0) override;

        Barrier*  CreateBarrier() override;
        void      DestroyBarrier(Barrier* barrier) override;
        void      WaitForJobs(Barrier* barrier) override;

    protected:
        void      QueueJob (Job*  job)                     override;
        void      QueueJobs(Job** jobs, JPH::uint numJobs) override;
        void      FreeJob  (Job*  job)                     override;

    private:
        static void TrampolineFn(Luth::JobSystem::JobArgs args);

        // PendingJob records a job currently tracked by a barrier — populated by AddJob, removed
        // by OnJobFinished, snapshotted by the watchdog when a wait gets stuck. Name is captured
        // from the side-table populated in CreateJob.
        struct PendingJob
        {
            Job*        job;
            std::string name;
        };

        class LuthBarrier final : public Barrier
        {
        public:
            LuthBarrier() = default;
            ~LuthBarrier() override = default;

            void AddJob (const JobHandle&  handle)                    override;
            void AddJobs(const JobHandle*  handles, JPH::uint num)    override;

            // Counter tracks pending jobs added to this barrier. Increment on AddJob (only when SetBarrier
            // succeeds — see .cpp), decrement on OnJobFinished. WaitForJobs routes here.
            Luth::JobSystem::AtomicCounter Counter;

            // Watchdog tracking. Pending mutates only under m_PendingMutex; IsWaiting + WaitStart
            // are read by the watchdog thread without locking against the wait fast-path.
            std::mutex                            m_PendingMutex;
            std::vector<PendingJob>               m_Pending;
            std::atomic<bool>                     m_IsWaiting { false };
            std::atomic<bool>                     m_HasLogged { false };
            std::chrono::steady_clock::time_point m_WaitStart;

            // Set by CreateBarrier so AddJob can resolve job names from the parent's side-table.
            LuthJobSystemForJolt*                 m_Parent = nullptr;

        protected:
            void OnJobFinished(Job* job) override;
        };

        JPH::FixedSizeFreeList<Job>               m_Jobs;
        std::vector<std::unique_ptr<LuthBarrier>> m_Barriers;
        std::mutex                                m_BarriersMutex;
        int                                       m_MaxConcurrency = 1;

        // Job-name side-table — keyed by Job*. CreateJob inserts; FreeJob removes; LuthBarrier::AddJob
        // looks up. Lets the watchdog name stuck jobs without forcing JPH_PROFILE_ENABLED on Jolt itself.
        std::unordered_map<Job*, std::string>     m_JobNames;
        std::mutex                                m_NameMapMutex;

        // Background diagnostic thread. Polls active barriers; logs detail when any has been waiting
        // beyond the threshold. Each barrier logs once per stuck-period (HasLogged latch), reset on
        // the next WaitForJobs entry.
        std::atomic<bool>                         m_WatchdogRunning { false };
        std::thread                               m_WatchdogThread;
        void WatchdogLoop();

        static constexpr auto kWatchdogThreshold  = std::chrono::milliseconds(1000);
        static constexpr auto kWatchdogPollPeriod = std::chrono::milliseconds(250);
    };
}
