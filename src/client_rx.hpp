#pragma once

#include "common.hpp"
#include "producer_queue.hpp"

void client_rx() {
    auto logger_client_rx = init_single_logger("client_rx");
    // spdlog::drop_all();
    // auto logger_client_rx = spdlog::rotating_logger_mt(
    //     "client_rx", get_source_directory() + "/../logs/client_rx.log",
    //     LOG_FILE_SIZE, LOG_FILE_EXTRA_NUM);
    // logger_client_rx->set_pattern(LOG_FORMAT);
    // logger_client_rx->set_level(LOG_LEVEL_PRODUCER);
    // logger_client_rx->flush_on(spdlog::level::debug);
    // logger_client_rx->info("client_rx running (PID: {})", getpid());

    ProducerQueue producer_queue("test");
    int i = 0;
    while (i < 100) {
        std::string message = "message " + std::to_string(i++);

        if (!producer_queue.sendMessage(message)) {
            producer_queue.get_logger()->info(
                "Message send failed - buffer is full\n");
        }
        logger_client_rx->info("client_rx running (PID: {}) - send message {}",
                               getpid(), i);
        sleep(1);
    }
    producer_queue.flushBatch();
}
