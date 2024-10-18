#pragma once

#include <spdlog/sinks/rotating_file_sink.h>
#include <spdlog/spdlog.h>

const int size_logfile = 5 * 1024 * 1024;  // 5MB
const int num_logfile = 3;                 // 3 rotations

void initialize_logger(std::shared_ptr<spdlog::logger> logger,
                       enum spdlog::level::level_enum log_level,
                       enum spdlog::level::level_enum flush_level);