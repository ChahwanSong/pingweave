#pragma once

#include <cstdarg>  // va_list, va_start, va_end
#include <cstring>  // strerror
#include <iostream>
#include <sstream>

#include "extlibs.hpp"
#include "macro.hpp"

// spdlog
const int LOG_FILE_SIZE = 10 * 1024 * 1024;  // 10 MB
const int LOG_FILE_EXTRA_NUM = 0;            // extra rotate-log files
const std::string LOG_FORMAT = "[%Y-%m-%d %H:%M:%S.%f][%l] %v";
const std::string LOG_RESULT_FORMAT = "%v";
const enum spdlog::level::level_enum LOG_LEVEL_SERVER = spdlog::level::debug;
const enum spdlog::level::level_enum LOG_LEVEL_CLIENT = spdlog::level::debug;
const enum spdlog::level::level_enum LOG_LEVEL_RESULT = spdlog::level::debug;
const enum spdlog::level::level_enum LOG_LEVEL_PING_TABLE =
    spdlog::level::debug;

inline std::string get_source_directory() {
#ifndef SOURCE_DIR
    // If missed, give a current directory
    return ".";
#else
    // SOURCE_DIR will be defined in Makefile
    return SOURCE_DIR;
#endif
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

inline std::shared_ptr<spdlog::logger> initialize_logger(
    const std::string &logname, const std::string &dir_path,
    enum spdlog::level::level_enum log_level, int file_size, int file_num) {
    auto logger = spdlog::get(logname);
    if (!logger) {
        logger = spdlog::rotating_logger_mt(
            logname, get_source_directory() + dir_path + "/" + logname + ".log",
            file_size, file_num);
        logger->set_pattern(LOG_FORMAT);
        logger->set_level(log_level);
        logger->flush_on(log_level);
        logger->info("Logger initialization (logname: {}, PID: {})", logname,
                     getpid());
    }
    return logger;
}