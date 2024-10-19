#pragma once

#include "common.hpp"

void server_rx() {
    spdlog::drop_all();
    auto logger_server_rx = spdlog::rotating_logger_mt(
        "server_rx", get_source_directory() + "/../logs/server_rx.log",
        LOG_FILE_SIZE, LOG_FILE_EXTRA_NUM);

    spdlog::get("server_rx")->info("server_rx running (PID: {})", getpid());

    while (true) {
        sleep(1);
    }
}
