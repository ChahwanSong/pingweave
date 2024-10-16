#pragma once

#include <spdlog/sinks/rotating_file_sink.h>
#include <spdlog/spdlog.h>

// create logger extern pointer
extern std::shared_ptr<spdlog::logger> queue_logger;

void initialize_queue_logger(const std::string& logname);