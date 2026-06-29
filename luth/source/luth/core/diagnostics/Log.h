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

#include <array>
#include <chrono>
#include <memory>
#include <string>
#include <string_view>
#include <cassert>

namespace Luth
{
    // spdlog wrapper plus the LH_CORE_* / LH_LOG macros every subsystem uses. ILogSink lets the
    // editor's ConsolePanel observe the log stream; OnLogEntry fires from worker / IO / main
    // threads, so implementations must be re-entrant. Sinks must NOT call LH_CORE_* from
    // OnLogEntry (the spdlog base_sink isn't recursive and would deadlock); ConsolePanel routes
    // through EventBus to drain on the main thread.
    enum class LogLevel : u8 {
        Trace, Debug, Info, Warn, Error, Critical, Off
    };

    // Subsystem channel a log line belongs to. Each maps to one spdlog logger (all sharing the
    // same sinks); the ConsolePanel filters per-category. Renderer / Shaders are verbose channels
    // (off by default in the console) — keep one-time engine bringup on Core so it stays visible.
    enum class LogCategory : u8 {
        Core, Assets, Shaders, Renderer, Jobs, Physics, Scene, Editor, Count
    };

    constexpr const char* LogCategoryName(LogCategory c)
    {
        switch (c) {
            case LogCategory::Core:     return "Core";
            case LogCategory::Assets:   return "Assets";
            case LogCategory::Shaders:  return "Shaders";
            case LogCategory::Renderer: return "Renderer";
            case LogCategory::Jobs:     return "Jobs";
            case LogCategory::Physics:  return "Physics";
            case LogCategory::Scene:    return "Scene";
            case LogCategory::Editor:   return "Editor";
            default:                    return "Core";
        }
    }

    struct LogEntry {
        LogLevel    level;
        LogCategory category = LogCategory::Core;
        std::string message;
        std::string logger;     // category name (spdlog logger_name)
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

        // One spdlog logger per category, all sharing the stdout/file/forwarding sinks.
        // Returns nullptr only before Init (main() calls Init first, so call sites are safe).
        inline static spdlog::logger* GetLogger(LogCategory cat) {
            return s_Loggers[static_cast<size_t>(cat)].get();
        }

        // Register / unregister an ILogSink. Safe across threads. Safe to call
        // before Init (sinks are retained; fan-out begins once spdlog is wired).
        static void AddSink(ILogSink* sink);
        static void RemoveSink(ILogSink* sink);

    private:
        static std::array<std::shared_ptr<spdlog::logger>, static_cast<size_t>(LogCategory::Count)> s_Loggers;
    };
}

// ── Logging macros ───────────────────────────────────────────────────────────
// LH_LOG(cat, method, ...) is the categorized entry point: `cat` is a bare
// LogCategory enumerator (Assets, Shaders, …), `method` the spdlog level method
// (trace/debug/info/warn/error/critical). LH_EXPAND re-tokenizes __VA_ARGS__ so
// the dispatch survives MSVC's traditional preprocessor (no /Zc:preprocessor).
#define FMT(...) fmt::format(__VA_ARGS__)
#define LH_EXPAND(x) x

#define LH_LOG(cat, method, ...) LH_EXPAND(LH_LOG_##method(::Luth::LogCategory::cat, __VA_ARGS__))

#define LH_LOG_info(c, ...)     ::Luth::Log::GetLogger(c)->info(FMT(__VA_ARGS__))
#define LH_LOG_warn(c, ...)     ::Luth::Log::GetLogger(c)->warn(FMT(__VA_ARGS__))
#define LH_LOG_error(c, ...)    ::Luth::Log::GetLogger(c)->error(FMT(__VA_ARGS__))
#define LH_LOG_critical(c, ...) ::Luth::Log::GetLogger(c)->critical(FMT(__VA_ARGS__))

// Trace/Debug compile out entirely in the shipping (Dist) build.
#if defined(LUTH_BUILD_DIST)
    #define LH_LOG_trace(c, ...) ((void)0)
    #define LH_LOG_debug(c, ...) ((void)0)
#else
    #define LH_LOG_trace(c, ...) ::Luth::Log::GetLogger(c)->trace(FMT(__VA_ARGS__))
    #define LH_LOG_debug(c, ...) ::Luth::Log::GetLogger(c)->debug(FMT(__VA_ARGS__))
#endif

// Core-category aliases — every existing LH_CORE_* call site keeps working. LH_CORE_DEBUG
// is new (the level/console always had Debug; nothing emitted it before).
#define LH_CORE_TRACE(...)    LH_LOG(Core, trace,    __VA_ARGS__)
#define LH_CORE_DEBUG(...)    LH_LOG(Core, debug,    __VA_ARGS__)
#define LH_CORE_INFO(...)     LH_LOG(Core, info,     __VA_ARGS__)
#define LH_CORE_WARN(...)     LH_LOG(Core, warn,     __VA_ARGS__)
#define LH_CORE_ERROR(...)    LH_LOG(Core, error,    __VA_ARGS__)
#define LH_CORE_CRITICAL(...) LH_LOG(Core, critical, __VA_ARGS__)


// Assert
#define LH_CORE_ASSERT(condition, ...)                              \
    do {                                                            \
        if (!(condition)) {                                         \
            LH_CORE_CRITICAL("Assertion Failed: {0}", __VA_ARGS__); \
            assert(false && #condition);                            \
        }                                                           \
    } while(0)
