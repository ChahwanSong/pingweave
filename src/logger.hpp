#pragma once

#include <spdlog/async.h>                     // spdlog
#include <spdlog/sinks/rotating_file_sink.h>  // spdlog
#include <spdlog/sinks/stdout_color_sinks.h>  // spdlog
#include <spdlog/spdlog.h>                    // spdlog

#include <cstdarg>  // va_list, va_start, va_end
#include <cstring>  // strerror
#include <iostream>
#include <sstream>

#ifndef SOURCE_DIR
#define SOURCE_DIR (".")
#endif

// spdlog
const int LOG_FILE_SIZE = 10 * 1024 * 1024;  // 10 MB
const int LOG_FILE_EXTRA_NUM = 0;            // only one rating-log file
const std::string LOG_FORMAT = "[%Y-%m-%d %H:%M:%S.%f][%l] %v";
const enum spdlog::level::level_enum LOG_LEVEL_PRODUCER = spdlog::level::trace;
const enum spdlog::level::level_enum LOG_LEVEL_SERVER = spdlog::level::trace;
const enum spdlog::level::level_enum LOG_LEVEL_CLIENT = spdlog::level::trace;

// get a absolute path of source directory
inline std::string get_source_directory() { return SOURCE_DIR; }

// check log message
inline bool check_log(std::string &ctx_log) { return !ctx_log.empty(); }

// append log message to ctx_log
inline void append_log(std::string &ctx_log, const char *format, ...) {
    char buffer[64];
    va_list args;
    va_start(args, format);
    vsnprintf(buffer, 64, format, args);
    va_end(args);
    ctx_log.append(buffer).append("\n");
}

std::shared_ptr<spdlog::logger> initialize_custom_logger(
    const std::string &logname, enum spdlog::level::level_enum log_level);
