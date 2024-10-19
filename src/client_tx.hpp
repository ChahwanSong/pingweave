#pragma once

#include "common.hpp"

void client_tx() {
    spdlog::drop_all();
    auto logger_client_tx = spdlog::rotating_logger_mt(
        "client_tx", get_source_directory() + "/../logs/client_tx.log",
        LOG_FILE_SIZE, LOG_FILE_EXTRA_NUM);
    logger_client_tx->set_pattern(LOG_FORMAT);
    logger_client_tx->set_level(LOG_LEVEL_PRODUCER);
    logger_client_tx->flush_on(spdlog::level::debug);

    spdlog::get("client_tx")->info("client_tx running (PID: {})", getpid());
    logger_client_tx->flush();
    while (true) {
        sleep(1);
    }
}
