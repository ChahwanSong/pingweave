#pragma once

#include <cstdarg>  // va_list, va_start, va_end
#include <cstring>  // strerror
#include <iostream>
#include <sstream>
#include <unordered_map>

#include "extlibs.hpp"
#include "macro.hpp"

// spdlog
const int LOG_FILE_SIZE = 30 * 1024 * 1024;  // 30 MB
const int LOG_FILE_EXTRA_NUM = 0;            // extra rotate-log files
const std::string LOG_FORMAT = "[%Y-%m-%d %H:%M:%S.%f][%l] %v";
const std::string LOG_RESULT_FORMAT = "%v";

static const std::unordered_map<std::string, enum spdlog::level::level_enum>
    logLevelMap = {{"trace", spdlog::level::trace},
                   {"TRACE", spdlog::level::trace},
                   {"debug", spdlog::level::debug},
                   {"DEBUG", spdlog::level::debug},
                   {"info", spdlog::level::info},
                   {"INFO", spdlog::level::info},
                   {"warn", spdlog::level::warn},
                   {"WARN", spdlog::level::warn},
                   {"error", spdlog::level::err},
                   {"ERROR", spdlog::level::err},
                   {"critical", spdlog::level::critical},
                   {"CRITICAL", spdlog::level::critical}};

inline std::shared_ptr<spdlog::logger> initialize_logger(
    const std::string &logname, const std::string &dir_path,
    const enum spdlog::level::level_enum &log_level, int file_size,
    int file_num) {
    auto logger = spdlog::get(logname);
    if (!logger) {
        logger = spdlog::rotating_logger_mt(
            logname, get_src_dir() + dir_path + "/" + logname + ".log",
            file_size, file_num);
        logger->set_pattern(LOG_FORMAT);
        logger->set_level(log_level);
        logger->flush_on(log_level);
        logger->info("Logger initialization (logname: {}, PID: {})", logname,
                     getpid());
    }
    return logger;
}