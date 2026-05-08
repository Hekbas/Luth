#pragma once

#include <Jolt/Jolt.h>
#include <Jolt/Core/JobSystem.h>
#include <Jolt/Core/FixedSizeFreeList.h>

#include "luth/jobs/JobSystem.h"
#include "luth/jobs/AtomicCounter.h"

#include <vector>
#include <memory>

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

        protected:
            void OnJobFinished(Job* job) override;
        };

        JPH::FixedSizeFreeList<Job>               m_Jobs;
        std::vector<std::unique_ptr<LuthBarrier>> m_Barriers;
        int                                       m_MaxConcurrency = 1;
    };
}
