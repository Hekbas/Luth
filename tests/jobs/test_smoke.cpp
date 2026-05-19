#include <doctest/doctest.h>

#include "luth/jobs/JobSystem.h"
#include "support/JobSystemFixture.h"

TEST_CASE_FIXTURE(LuthTests::JobSystemFixture, "Smoke: JobSystem init+shutdown succeeds [smoke]")
{
    auto stats = Luth::JobSystem::GetStats();
    CHECK(stats.ThreadCount > 0);
    CHECK(stats.TotalFibers > 0);
}
