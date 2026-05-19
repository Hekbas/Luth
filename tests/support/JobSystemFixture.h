#pragma once

#include "luth/jobs/JobSystem.h"

namespace LuthTests
{
    // Per-test RAII wrapper around JobSystem::Init/Shutdown. JobSystem holds static
    // s_Data plus a Win32 FLS slot — calling Init twice without Shutdown leaks the FLS
    // index and destroys live std::thread objects. One fixture instance per TEST_CASE
    // keeps each test's fiber pool isolated so failures bisect cleanly.
    struct JobSystemFixture
    {
        explicit JobSystemFixture(Luth::u32 numWorkers = 0)
        {
            Luth::JobSystem::Init(numWorkers);
        }

        ~JobSystemFixture()
        {
            Luth::JobSystem::Shutdown();
        }

        JobSystemFixture(const JobSystemFixture&)            = delete;
        JobSystemFixture& operator=(const JobSystemFixture&) = delete;
    };
}
