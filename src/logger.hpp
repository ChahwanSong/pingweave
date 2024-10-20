#pragma once

#include <spdlog/async.h>                     // spdlog
#include <spdlog/sinks/rotating_file_sink.h>  // spdlog
#include <spdlog/sinks/stdout_color_sinks.h>  // spdlog
#include <spdlog/spdlog.h>                    // spdlog

#ifndef SOURCE_DIR
#define SOURCE_DIR (".")
#endif

// spdlog
const int LOG_FILE_SIZE = 10 * 1024 * 1024;  // 10 MB
const int LOG_FILE_EXTRA_NUM = 0;            // extra 3 rotations
const std::string LOG_FORMAT = "[%Y-%m-%d %H:%M:%S.%e][%l] %v";
const enum spdlog::level::level_enum LOG_LEVEL_MAIN = spdlog::level::trace;
const enum spdlog::level::level_enum LOG_LEVEL_PRODUCER = spdlog::level::trace;
const enum spdlog::level::level_enum LOG_LEVEL_SERVER = spdlog::level::trace;
const enum spdlog::level::level_enum LOG_LEVEL_CLIENT = spdlog::level::trace;

std::string get_source_directory();

std::shared_ptr<spdlog::logger> initialize_custom_logger(
    const std::string &logname, enum spdlog::level::level_enum log_level);

inline std::shared_ptr<spdlog::logger> logger() {
    static auto logger = spdlog::stdout_color_mt("console");
    spdlog::set_default_logger(logger);
    spdlog::set_pattern(LOG_FORMAT);
    spdlog::set_level(LOG_LEVEL_MAIN);
    return logger;
}