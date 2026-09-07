#pragma once

#include "luth/core/diagnostics/Log.h"
#include "luth/jobs/SpinLock.h"

#include <filesystem>
#include <vector>

namespace Luth
{
    // Captures the log stream to a JSON array for offline consumers (LuthShot), so a harness run can be
    // diagnosed from its output file instead of scraped from stdout.
    //
    // invariant: OnLogEntry fires from worker / IO / main threads and must never log, so it only appends
    // under a SpinLock; all formatting and file IO happens in Flush at shutdown. see Log.h.
    class JsonLogSink : public ILogSink
    {
    public:
        void OnLogEntry(const LogEntry& entry) override;

        // Writes the captured entries and unregisters. Safe to call once; later calls are no-ops.
        bool Flush(const std::filesystem::path& path);

        u32 ErrorCount() const { return m_Errors; }

    private:
        mutable SpinLock      m_Lock;
        std::vector<LogEntry> m_Entries;
        u32                   m_Errors = 0;
    };
}
