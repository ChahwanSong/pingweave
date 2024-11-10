#include "logger.hpp"

std::shared_ptr<spdlog::logger> initialize_custom_logger(
    const std::string &logname, enum spdlog::level::level_enum log_level,
    int file_size, int file_num) {
    auto logger = spdlog::get(logname);
    if (!logger) {
        logger = spdlog::rotating_logger_mt(
            logname, get_source_directory() + DIR_LOG_PATH + logname + ".log",
            file_size, file_num);
        logger->set_pattern(LOG_FORMAT);
        logger->set_level(log_level);
        logger->flush_on(spdlog::level::info); /** TODO: */
        logger->info("Logger initialization (logname: {}, PID: {})", logname,
                     getpid());
    }
    return logger;
}

std::shared_ptr<spdlog::logger> initialize_result_logger(
    const std::string &logname, enum spdlog::level::level_enum log_level,
    int file_size, int file_num) {
    auto logger = spdlog::get(logname);
    if (!logger) {
        logger = spdlog::rotating_logger_mt(
            logname,
            get_source_directory() + DIR_RESULT_PATH + logname + ".log",
            file_size, file_num);
        logger->set_pattern(LOG_RESULT_FORMAT);
        logger->set_level(log_level);
        // logger->flush_on(log_level); /** TODO: */
        logger->debug("Result logger initialization (logname: {}, PID: {})",
                      logname, getpid());
    }
    return logger;
}