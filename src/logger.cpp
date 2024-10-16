#include "logger.hpp"

// queue_logger의 정의
std::shared_ptr<spdlog::logger> queue_logger = nullptr;

// call at main function
void initialize_queue_logger(const std::string& logname) {
    size_t max_file_size = 5 * 1024 * 1024;  // 5MB
    size_t max_files = 3;                    // 최대 파일 개수

    // message format
    spdlog::set_pattern("[%Y-%m-%d %H:%M:%S][%l][%s:%#] %v");

    // logging with a ring buffer
    queue_logger = spdlog::rotating_logger_mt(
        logname, "../logs/" + logname + ".log", max_file_size, max_files);

    // set log level
    queue_logger->set_level(spdlog::level::debug);

    // Automatically flush every time an info-level or higher message is logged
    queue_logger->flush_on(spdlog::level::info);

    queue_logger->info("Logger initialized is successful.");
}