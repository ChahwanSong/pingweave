#include "logger.hpp"

std::shared_ptr<spdlog::logger> initialize_custom_logger(
    const std::string &logname, enum spdlog::level::level_enum log_level) {
    spdlog::drop_all();
    auto logger = spdlog::rotating_logger_mt(
        logname, get_source_directory() + "/../logs/" + logname + ".log",
        LOG_FILE_SIZE, LOG_FILE_EXTRA_NUM);
    logger->set_pattern(LOG_FORMAT);
    logger->set_level(log_level);
    logger->flush_on(log_level);
    logger->info("Logger initialization (logname: {}, PID: {})", logname,
                 getpid());
    return logger;
}
