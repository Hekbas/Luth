// doctest entry for LuthTests.
// doctest single-header vendored at tests/extern/doctest/doctest.h
// (commit history records the pinned upstream tag + download URL).

#define DOCTEST_CONFIG_IMPLEMENT
#include <doctest/doctest.h>

#include "luth/core/diagnostics/Log.h"

int main(int argc, char** argv)
{
    Luth::Log::Init();

    doctest::Context ctx(argc, argv);
    return ctx.run();
}
