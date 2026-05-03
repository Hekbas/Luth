#include "luthpch.h"
#include "luth/core/diagnostics/Log.h"
#include "luth/jobs/SpinLock.h"

#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/sinks/basic_file_sink.h>
#include <spdlog/sinks/base_sink.h>

#include <algorithm>
#include <vector>

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
                entry.timestamp = msg.time;

                // Snapshot the list under the spinlock so observer callbacks can safely
                // call AddSink/RemoveSink (their lock acquisition won't recurse on us).
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

    std::shared_ptr<spdlog::logger> Log::s_Logger;

    void Log::Init()
    {
        std::vector<spdlog::sink_ptr> sinks;
        sinks.emplace_back(std::make_shared<spdlog::sinks::stdout_color_sink_mt>());
        sinks.emplace_back(std::make_shared<spdlog::sinks::basic_file_sink_mt>("Luth.log", true));
        sinks.emplace_back(std::make_shared<ForwardingSink>());

        s_Logger = std::make_shared<spdlog::logger>("LUTH", begin(sinks), end(sinks));
        spdlog::register_logger(s_Logger);
        s_Logger->set_pattern("%^[%T] %n: %v%$");  // Timestamp, logger name, message
        s_Logger->set_level(spdlog::level::trace);
        s_Logger->flush_on(spdlog::level::trace);
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
