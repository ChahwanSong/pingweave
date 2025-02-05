#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <cstring>
#include <iostream>

#include "../common.hpp"
#include "../tcpudp_common.hpp"
#include "../tcpudp_ping_info.hpp"

// ./client 10.200.200.3 10.200.200.2 7227

#define PING_INTERVAL_US (1000)

void udp_client_tx_thread(struct udp_context* ctx_tx,
                          const std::string& server_ip,
                          const uint64_t& server_port,
                          TcpUdpPinginfoMap* ping_table,
                          std::shared_ptr<spdlog::logger> logger) {
    logger->info("[TX] Running (Thread ID: {})...", get_thread_id());

    // simple version
    uint64_t ping_uid = 0;

    while (true) {
        // Create a pingid
        auto pingid = make_pingid(ip2uint(server_ip), ping_uid++);

        // Record the send time
        if (!ping_table->insert(pingid, pingid, server_ip)) {
            logger->warn("[TX] Failed to insert ping ID {} into ping_table.",
                         pingid);
        }

        // send
        if (send_udp_message(ctx_tx, server_ip, server_port, pingid,
                             spdlog::default_logger())) {
            // somethign wrong
            logger->warn("[TX] Failed to send response to {}", server_ip);
            continue;
        }

        usleep(PING_INTERVAL_US);  // 0.001초마다 반복
    }
}

void udp_client_rx_thread(struct udp_context* ctx_rx,
                          TcpUdpPinginfoMap* ping_table,
                          std::shared_ptr<spdlog::logger> logger) {
    logger->info("Running RX thread (Thread ID: {})...", get_thread_id());

    try {
        while (true) {
            // Wait for the next UDP message event
            uint64_t pingid = 0;
            uint64_t recv_time_steady;
            std::string sender;
            if (receive_udp_message(ctx_rx, pingid, sender, recv_time_steady,
                                    logger)) {
                // something wrong
                continue;
            }

            if (!ping_table->update_pong_info(pingid, recv_time_steady)) {
                logger->warn("[RX] PONG ({}): No entry in ping_table", pingid);
                continue;
            }
        }
    } catch (const std::exception& e) {
        logger->error("[RX] Exception in RX thread: {}", e.what());
        throw;  // Propagate exception`
    }
}

void udp_client_result_thread(TcpUdpClientQueue* client_queue,
                              std::shared_ptr<spdlog::logger> logger) {
    // msg from RX Thread
    struct tcpudp_result_msg_t result_msg;

    int n = 0;
    double max_rtt = 0;

    /** RESULT: (dstip, #success, #failure, #weird, mean, max, p50, p95, p99) */
    try {
        while (true) {
            // fully-blocking with timeout
            if (client_queue->wait_dequeue_timed(
                    result_msg,
                    std::chrono::milliseconds(WAIT_DEQUEUE_TIME_MS))) {
                logger->debug("{}, {}, {}, {}, {}",
                              timestamp_ns_to_string(result_msg.time_ping_send),
                              uint2ip(result_msg.dstip), result_msg.pingid,
                              result_msg.network_delay, result_msg.result);
            }

            auto rtt_ms = result_msg.network_delay / 1000000.0;
            n++;
            max_rtt = max_rtt > rtt_ms ? max_rtt : rtt_ms;

            if (rtt_ms >= 1) {
                spdlog::info("RTT: {} ms", rtt_ms);
            }

            if (n % (1000000/PING_INTERVAL_US) == 0 && n > 0) {
                spdlog::info("Max RTT: {} ms over {} trials...", max_rtt, n);
                max_rtt = 0;
                n = 0;
            }

        }
    } catch (const std::exception& e) {
        logger->error("Exception in result_thread: {}", e.what());
        throw;
    }
}

int main(int argc, char* argv[]) {
    if (argc < 4) {
        std::cerr << "Usage: " << argv[0]
                  << " <서버_IP> <클라이언트_IP> <서버_PORT> \n";
        return 1;
    }

    const char* server_ip = argv[1];
    const char* client_ip = argv[2];
    const int server_port = std::stoi(argv[3]);

    // Initialize UDP contexts
    udp_context ctx_tx, ctx_rx;
    if (initialize_contexts(ctx_tx, ctx_rx, client_ip,
                            spdlog::default_logger())) {
        throw std::runtime_error("Failed to initialize UDP contexts.");
    }

    TcpUdpClientQueue client_queue(MSG_QUEUE_SIZE);
    TcpUdpPinginfoMap ping_table(spdlog::default_logger(), &client_queue,
                                 PINGWEAVE_TABLE_EXPIRY_TIME_UDP_MS);

    // Start the RX thread
    std::thread result_thread(udp_client_result_thread, &client_queue,
                              spdlog::default_logger());

    std::thread rx_thread(udp_client_rx_thread, &ctx_rx, &ping_table,
                          spdlog::default_logger());

    // Start the TX thread
    std::thread tx_thread(udp_client_tx_thread, &ctx_tx, server_ip, server_port,
                          &ping_table, spdlog::default_logger());

    // termination
    if (result_thread.joinable()) {
        result_thread.join();
    }

    if (tx_thread.joinable()) {
        tx_thread.join();
    }

    if (rx_thread.joinable()) {
        rx_thread.join();
    }

    // // simple version
    // uint64_t pingid = 0;
    // int n = 0;
    // double max_rtt = 0;
    // while (true) {
    //     auto start_time = std::chrono::high_resolution_clock::now();

    //     // send
    //     if (send_udp_message(&ctx_tx, server_ip, server_port, ++pingid,
    //                          spdlog::default_logger())) {
    //         // somethign wrong
    //         spdlog::warn("Failed to send response to {}", server_ip);
    //         continue;
    //     }

    //     // recv
    //     std::string sender;
    //     uint64_t recv_time_steady;
    //     if (receive_udp_message(&ctx_rx, pingid, sender, recv_time_steady,
    //                             spdlog::default_logger())) {
    //         // receive_message 실패 시 처리
    //         spdlog::warn("receive_message failed");
    //         continue;
    //     }

    //     auto end_time = std::chrono::high_resolution_clock::now();
    //     std::chrono::duration<double, std::milli> rtt = end_time -
    //     start_time;

    //     n++;
    //     max_rtt = max_rtt > rtt.count() ? max_rtt : rtt.count();

    //     if (rtt.count() >= 1) {
    //         spdlog::info("RTT: {} ms", rtt.count());
    //     }

    //     if (n % (1000000/PING_INTERVAL_US) == 0 && n > 0) {
    //         spdlog::info("Max RTT: {} ms over {} trials...", max_rtt, n);
    //         max_rtt = 0;
    //         n = 0;
    //     }

    //     usleep(PING_INTERVAL_US);  // 0.001초마다 반복
    // }
}