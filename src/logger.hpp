#pragma once

#include <cstdarg>  // va_list, va_start, va_end
#include <cstring>  // strerror
#include <iostream>
#include <sstream>

#include "libs_external.hpp"
#include "macro.hpp"

#ifndef SOURCE_DIR
#define SOURCE_DIR (".")
#endif

// spdlog
const int LOG_FILE_SIZE = 10 * 1024 * 1024;  // 10 MB
const int LOG_FILE_EXTRA_NUM = 0;            // only one rating-log file
const std::string LOG_FORMAT = "[%Y-%m-%d %H:%M:%S.%f][%l] %v";
const std::string LOG_RESULT_FORMAT = "%v";
const enum spdlog::level::level_enum LOG_LEVEL_PRODUCER = spdlog::level::debug;
const enum spdlog::level::level_enum LOG_LEVEL_SERVER = spdlog::level::info;
const enum spdlog::level::level_enum LOG_LEVEL_CLIENT = spdlog::level::info;
const enum spdlog::level::level_enum LOG_LEVEL_RESULT = spdlog::level::info;
const enum spdlog::level::level_enum LOG_LEVEL_PING_TABLE = spdlog::level::info;

// get a absolute path of source directory
inline std::string get_source_directory() { return SOURCE_DIR; }

std::shared_ptr<spdlog::logger> initialize_custom_logger(
    const std::string &logname, enum spdlog::level::level_enum log_level,
    int file_size, int file_num);

std::shared_ptr<spdlog::logger> initialize_result_logger(
    const std::string &logname, enum spdlog::level::level_enum log_level,
    int file_size, int file_num);

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

// calculate time difference with considering bit wrap-around
inline uint64_t calc_time_delta_with_bitwrap(const uint64_t &t1,
                                             const uint64_t &t2,
                                             const uint64_t &mask) {
    uint64_t delta;
    if (t2 >= t1) {  // no wrap around
        delta = t2 - t1;
    } else {  // wrap around
        delta = (mask - t1 + 1) + t2;
    }
    return delta;
}
