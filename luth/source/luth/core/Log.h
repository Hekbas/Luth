#pragma once

// spdlog active level
#if defined(_DEBUG) || defined(DEBUG)
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

#include <memory>
#include <cassert>

namespace Luth
{
    class Log
    {
    public:
        static void Init();
        inline static std::shared_ptr<spdlog::logger>& GetLogger() { return s_Logger; }

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
