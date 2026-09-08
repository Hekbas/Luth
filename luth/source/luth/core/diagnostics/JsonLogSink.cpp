#include "luthpch.h"
#include "luth/core/diagnostics/JsonLogSink.h"

namespace Luth
{
    namespace
    {
        constexpr const char* LevelName(LogLevel l)
        {
            switch (l) {
                case LogLevel::Trace:    return "trace";
                case LogLevel::Debug:    return "debug";
                case LogLevel::Info:     return "info";
                case LogLevel::Warn:     return "warn";
                case LogLevel::Error:    return "error";
                case LogLevel::Critical: return "critical";
                default:                 return "off";
            }
        }
    }

    void JsonLogSink::OnLogEntry(const LogEntry& entry)
    {
        SpinLockGuard guard(m_Lock);
        if (entry.level == LogLevel::Error || entry.level == LogLevel::Critical)
            ++m_Errors;
        m_Entries.push_back(entry);
    }

    bool JsonLogSink::Flush(const std::filesystem::path& path)
    {
        // Detach first: serializing below is free to allocate and log, which would otherwise re-enter.
        Log::RemoveSink(this);

        std::vector<LogEntry> entries;
        {
            SpinLockGuard guard(m_Lock);
            entries.swap(m_Entries);
        }

        nlohmann::json arr = nlohmann::json::array();
        for (const LogEntry& e : entries)
        {
            const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                e.timestamp.time_since_epoch()).count();
            arr.push_back({
                { "level",     LevelName(e.level) },
                { "category",  LogCategoryName(e.category) },
                { "message",   e.message },
                { "timestamp", ms }
            });
        }

        const auto parent = path.parent_path();
        if (!parent.empty()) {
            std::error_code ec;
            std::filesystem::create_directories(parent, ec);
        }

        std::ofstream out(path);
        if (!out) return false;
        out << arr.dump(2);
        return out.good();
    }
}
