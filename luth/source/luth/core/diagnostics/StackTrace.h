#pragma once

#include <string>
#include <vector>

namespace Luth::StackTrace
{
    // Capture the current call stack as formatted "  symbol (file:line)" frames.
    // skip = how many top frames to drop (caller + helpers); max caps the result.
    // On non-Windows builds returns an empty vector. DbgHelp is single-threaded, so
    // the impl serialises symbol resolution behind a SpinLock.
    std::vector<std::string> Capture(int skip = 0, int max = 32);

    // Capture and emit each frame via LH_CORE_ERROR. Convenience for catch blocks.
    void LogStackTrace(int skip = 0, int max = 32);
}
