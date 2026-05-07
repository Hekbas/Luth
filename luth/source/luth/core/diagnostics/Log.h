#pragma once

// spdlog active level
#if defined(LUTH_BUILD_DEBUG)
#define SPDLOG_ACTIVE_LEVEL SPDLOG_LEVEL_TRACE
#else
#define SPDLOG_ACTIVE_LEVEL SPDLOG_LEVEL_INFO
#endif

// Disable NTTP (for pre-C++20)
//#define FMT_USE_NONTYPE_TEMPLATE_ARGS 0
//#define FMT_HEADER_ONLY 1

// Ignore warnings
#pragma warning(push, 0)
#include <spdlog/spdlog.h>
#include <spdlog/fmt/fmt.h>
#include <spdlog/fmt/ostr.h>
#pragma warning(pop)

#include "luth/core/types/LuthTypes.h"

#include <chrono>
#include <memory>
#include <string>
#include <cassert>

namespace Luth
{
    // spdlog wrapper plus the LH_CORE_* macros every subsystem uses. ILogSink lets the editor's
    // ConsolePanel observe the log stream; OnLogEntry fires from worker / IO / main threads, so
    // implementations must be re-entrant. Sinks must NOT call LH_CORE_* from OnLogEntry (the
    // spdlog base_sink isn't recursive and would deadlock); ConsolePanel routes through EventBus
    // to drain on the main thread.
    enum class LogLevel : u8 {
        Trace, Debug, Info, Warn, Error, Critical, Off
    };

    struct LogEntry {
        LogLevel level;
        std::string message;
        std::string logger;
        std::chrono::system_clock::time_point timestamp;
    };

    // Editor-side observer of the engine log stream. OnLogEntry fires from any
    // thread (workers / IO / main) — implementations must be re-entrant and MUST
    // NOT call LH_CORE_* from OnLogEntry (base_sink is non-recursive, deadlock).
    // ConsolePanel forwards via EventBus to drain on main.
    class ILogSink {
    public:
        virtual ~ILogSink() = default;
        virtual void OnLogEntry(const LogEntry& entry) = 0;
    };

    class Log
    {
    public:
        static void Init();
        inline static std::shared_ptr<spdlog::logger>& GetLogger() { return s_Logger; }

        // Register / unregister an ILogSink. Safe across threads. Safe to call
        // before Init (sinks are retained; fan-out begins once spdlog is wired).
        static void AddSink(ILogSink* sink);
        static void RemoveSink(ILogSink* sink);

    private:
        static std::shared_ptr<spdlog::logger> s_Logger;
    };
}

// Core logging macros
#define FMT(...) fmt::format(__VA_ARGS__)

#define LH_CORE_TRACE(...)    ::Luth::Log::GetLogger()->trace(FMT(__VA_ARGS__))
#define LH_CORE_INFO(...)     ::Luth::Log::GetLogger()->info(FMT(__VA_ARGS__))
#define LH_CORE_WARN(...)     ::Luth::Log::GetLogger()->warn(FMT(__VA_ARGS__))
#define LH_CORE_ERROR(...)    ::Luth::Log::GetLogger()->error(FMT(__VA_ARGS__))
#define LH_CORE_CRITICAL(...) ::Luth::Log::GetLogger()->critical(FMT(__VA_ARGS__))


// Assert
#define LH_CORE_ASSERT(condition, ...)                              \
    do {                                                            \
        if (!(condition)) {                                         \
            LH_CORE_CRITICAL("Assertion Failed: {0}", __VA_ARGS__); \
            assert(false && #condition);                            \
        }                                                           \
    } while(0)
