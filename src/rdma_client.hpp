#pragma once

#include "producer_queue.hpp"
#include "rdma_common.hpp"

void rdma_client(const std::string& ip_addr) {
    // init logger
    const std::string logname = "rdma_client_" + ip_addr;
    auto logger = initialize_custom_logger(logname, LOG_LEVEL_CLIENT);

    // init inter-process queue
    ProducerQueue producer_queue("rdma", ip_addr);

    while (true) {
        logger->info("Waiting 1 seconds...");
        sleep(1);
    }
}

// int i = 0;
// while (i < 5) {
//     std::string message = "message " + std::to_string(i++);

//     if (!producer_queue.sendMessage(message)) {
//         producer_queue.get_logger()->info(
//             "Message send failed - buffer is full\n");
//     }
//     logger->info("client_rx running (PID: {}) - send message
//     {}",
//                            getpid(), i);
// }
// producer_queue.flushBatch();