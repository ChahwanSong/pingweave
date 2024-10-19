#pragma once

#include "common.hpp"

void server_tx() {
    spdlog::drop_all();
    auto logger_server_tx = spdlog::rotating_logger_mt(
        "server_tx", get_source_directory() + "/../logs/server_tx.log",
        LOG_FILE_SIZE, LOG_FILE_EXTRA_NUM);

    spdlog::get("server_tx")->info("server_tx running (PID: {})", getpid());

    while (true) {
        sleep(1);
    }
}
