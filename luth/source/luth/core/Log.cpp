#include "luthpch.h"
#include "luth/core/Log.h"

#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/sinks/basic_file_sink.h>

namespace Luth
{
    std::shared_ptr<spdlog::logger> Log::s_Logger;

    void Log::Init()
    {
        std::vector<spdlog::sink_ptr> sinks;
        sinks.emplace_back(std::make_shared<spdlog::sinks::stdout_color_sink_mt>());
        sinks.emplace_back(std::make_shared<spdlog::sinks::basic_file_sink_mt>("Luth.log", true));

        s_Logger = std::make_shared<spdlog::logger>("LUTH", begin(sinks), end(sinks));
        spdlog::register_logger(s_Logger);
        s_Logger->set_pattern("%^[%T] %n: %v%$");  // Timestamp, logger name, message
        s_Logger->set_level(spdlog::level::trace);
        s_Logger->flush_on(spdlog::level::trace);
    }
}
