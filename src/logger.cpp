#include "logger.hpp"

void initialize_logger(std::shared_ptr<spdlog::logger> logger,
                       enum spdlog::level::level_enum log_level,
                       enum spdlog::level::level_enum flush_level) {
    // message format
    spdlog::set_pattern("[%Y-%m-%d %H:%M:%S][%l][%s:%#] %v");

    // set log level
    logger->set_level(log_level);

    // Automatically flush every time an info-level or higher message is logged
    logger->flush_on(flush_level);

    logger->info("Logger initialized is successful.");
}