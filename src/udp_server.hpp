#pragma once

#include "udp_common.hpp"
#include "udp_ping_info.hpp"

/**
 * NOTE: UDP Server is simple. It receives a message, and respond to it.
 * As there is no special mechanism, the logic is implemented in one function.
 */

// UDP server main function
void udp_server(const std::string& ipv4) {
    // Initialize logger
    const std::string server_logname = "udp_server_" + ipv4;
    enum spdlog::level::level_enum log_level_server;
    std::shared_ptr<spdlog::logger> server_logger;
    if (get_log_config_from_ini(log_level_server,
                                "logger_cpp_process_udp_server")) {
        server_logger =
            initialize_logger(server_logname, DIR_LOG_PATH, log_level_server,
                              LOG_FILE_SIZE, LOG_FILE_EXTRA_NUM);
        server_logger->info("UDP Server is running on pid {}", getpid());
    } else {
        throw std::runtime_error(
            "Failed to get a param 'logger_cpp_process_udp_server'");
    }

    // Initialize RDMA context
    udp_context ctx_server;
    if (make_ctx(&ctx_server, ipv4, PINGWEAVE_UDP_PORT_SERVER, server_logger)) {
        server_logger->error("Failed to create TX context for IP: {}", ipv4);
        throw std::runtime_error("Failed to create UDP context at server");
    }

    // logging the address
    if (ctx_server.sock && *ctx_server.sock >= 0) {
        log_bound_address(*ctx_server.sock, server_logger);
    }

    const int THRESHOLD_CONSECUTIVE_FAILURE = 5;
    int consecutive_failures = 0;  // 연속 실패 횟수 추적

    while (true) {
        uint64_t pingid = 0;
        std::string addr_msg_from;
        if (receive_message(&ctx_server, pingid, addr_msg_from,
                            server_logger)) {
            // receive_message 실패 시 처리
            consecutive_failures++;
            server_logger->warn("receive_message failed ({} times in a row)",
                                consecutive_failures);

            // wait 1 second if 5 consecutive failures
            if (consecutive_failures >= THRESHOLD_CONSECUTIVE_FAILURE) {
                server_logger->error(
                    "Too many ({}) consecutive receive failures. Waiting 1 "
                    "second before retry...",
                    THRESHOLD_CONSECUTIVE_FAILURE);
                std::this_thread::sleep_for(std::chrono::seconds(1));
                consecutive_failures = 0;
            }

            continue;
        }

        if (send_message(&ctx_server, addr_msg_from, PINGWEAVE_UDP_PORT_CLIENT,
                         pingid, server_logger)) {
            // somethign wrong
            server_logger->warn("Failed to send response to {}", addr_msg_from);
            continue;
        }
    }
}