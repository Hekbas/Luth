#include "luthpch.h"
#include "luth/core/diagnostics/Log.h"
#include "luth/jobs/SpinLock.h"

#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/sinks/basic_file_sink.h>
#include <spdlog/sinks/base_sink.h>

#include <algorithm>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#endif

namespace Luth
{
    namespace {
        SpinLock               s_SinkLock;
        std::vector<ILogSink*> s_Sinks;

        constexpr LogLevel SpdlogToLuth(spdlog::level::level_enum lvl)
        {
            switch (lvl) {
                case spdlog::level::trace:    return LogLevel::Trace;
                case spdlog::level::debug:    return LogLevel::Debug;
                case spdlog::level::info:     return LogLevel::Info;
                case spdlog::level::warn:     return LogLevel::Warn;
                case spdlog::level::err:      return LogLevel::Error;
                case spdlog::level::critical: return LogLevel::Critical;
                default:                      return LogLevel::Off;
            }
        }

        // Reverse of LogCategoryName: each per-category logger is named after its category, so the
        // forwarding sink recovers the channel from msg.logger_name. O(Count) compare; negligible at log frequency.
        LogCategory CategoryFromName(std::string_view name)
        {
            for (size_t i = 0; i < static_cast<size_t>(LogCategory::Count); ++i)
                if (name == LogCategoryName(static_cast<LogCategory>(i)))
                    return static_cast<LogCategory>(i);
            return LogCategory::Core;
        }

        // spdlog sink that fans engine log emissions out to registered Luth::ILogSink
        // observers. Reads the raw payload (msg.payload is the user's formatted message
        // before any pattern is applied), so observers receive the message as written.
        class ForwardingSink : public spdlog::sinks::base_sink<std::mutex>
        {
        protected:
            void sink_it_(const spdlog::details::log_msg& msg) override
            {
                LogEntry entry;
                entry.level     = SpdlogToLuth(msg.level);
                entry.message.assign(msg.payload.data(), msg.payload.size());
                entry.logger.assign(msg.logger_name.data(), msg.logger_name.size());
                entry.category  = CategoryFromName(std::string_view(msg.logger_name.data(), msg.logger_name.size()));
                entry.timestamp = msg.time;

                // Snapshot the list under the spinlock so observer callbacks can safely call
                // AddSink/RemoveSink (their lock acquisition won't recurse on the held lock).
                std::vector<ILogSink*> snapshot;
                {
                    SpinLockGuard guard(s_SinkLock);
                    snapshot = s_Sinks;
                }
                for (ILogSink* s : snapshot) {
                    s->OnLogEntry(entry);
                }
            }
            void flush_() override {}
        };
    }

    std::array<std::shared_ptr<spdlog::logger>, static_cast<size_t>(LogCategory::Count)> Log::s_Loggers;

    void Log::Init()
    {
#ifdef _WIN32
        // Engine sources compile with /utf-8, so string literals are UTF-8 bytes. Without this
        // the console reads them as the legacy OEM page and non-ASCII (em-dash etc.) becomes mojibake.
        SetConsoleOutputCP(CP_UTF8);
        SetConsoleCP(CP_UTF8);
#endif

        // Three shared sinks, each with its own level. The logger stays at trace so nothing is
        // pre-filtered before the file/forwarding sinks; the stdout sink alone curates to INFO,
        // keeping a clean console while Luth.log + the editor console retain full detail.
        auto stdoutSink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
        stdoutSink->set_level(spdlog::level::info);
        auto fileSink = std::make_shared<spdlog::sinks::basic_file_sink_mt>("Luth.log", true);
        fileSink->set_level(spdlog::level::trace);
        auto fwdSink = std::make_shared<ForwardingSink>();
        fwdSink->set_level(spdlog::level::trace);

        std::vector<spdlog::sink_ptr> sinks{ stdoutSink, fileSink, fwdSink };

        // One logger per category, all sharing the three sinks. Logger name == category name,
        // so stdout's %n shows the channel and the forwarding sink can recover the category.
        for (size_t i = 0; i < static_cast<size_t>(LogCategory::Count); ++i) {
            auto logger = std::make_shared<spdlog::logger>(
                LogCategoryName(static_cast<LogCategory>(i)), begin(sinks), end(sinks));
            logger->set_pattern("%^[%T] %n: %v%$");  // Timestamp, category, message
            logger->set_level(spdlog::level::trace);
            logger->flush_on(spdlog::level::trace);
            s_Loggers[i] = logger;
        }
    }

    void Log::AddSink(ILogSink* sink)
    {
        if (!sink) return;
        SpinLockGuard guard(s_SinkLock);
        s_Sinks.push_back(sink);
    }

    void Log::RemoveSink(ILogSink* sink)
    {
        if (!sink) return;
        SpinLockGuard guard(s_SinkLock);
        s_Sinks.erase(std::remove(s_Sinks.begin(), s_Sinks.end(), sink), s_Sinks.end());
    }
}
