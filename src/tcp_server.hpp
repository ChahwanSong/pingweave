#pragma once

#include "tcpudp_common.hpp"
#include "tcpudp_ping_info.hpp"


// TCP server main function
void tcp_server(const std::string& ipv4, const std::string& protocol) {
    // Initialize logger
    const std::string server_logname = protocol + "_server_" + ipv4;
    enum spdlog::level::level_enum log_level_server;
    std::shared_ptr<spdlog::logger> server_logger;
    if (get_log_config_from_ini(log_level_server,
                                "logger_cpp_process_tcp_server")) {
        server_logger =
            initialize_logger(server_logname, DIR_LOG_PATH, log_level_server,
                              LOG_FILE_SIZE, LOG_FILE_EXTRA_NUM);
        server_logger->info("TCP Server is running on pid {}", getpid());
    } else {
        throw std::runtime_error(
            "Failed to get a param 'logger_cpp_process_tcp_server'");
    }

    // Initialize TCP context
    tcp_context ctx_server;
    if (make_ctx(&ctx_server, ipv4, PINGWEAVE_TCP_PORT_SERVER, server_logger)) {
        server_logger->error("Failed to create TX context for IP: {}", ipv4);
        throw std::runtime_error("Failed to create TCP context at server");
    }

    // logging the address
    if (ctx_server.sock && *ctx_server.sock >= 0) {
        log_bound_address(*ctx_server.sock, server_logger);
    }

    int consecutive_failures = 0;  // 연속 실패 횟수 추적

    sockaddr_in newSocketInfo;
    socklen_t newSocketInfoLength = sizeof(newSocketInfo);
    int newSocketFileDescriptor;

    while (true) {
        server_logger->debug("Waiting a new TCP connection...");
        newSocketFileDescriptor = accept(*ctx_server.sock, (sockaddr *)&newSocketInfo, &newSocketInfoLength);
        if (newSocketFileDescriptor == -1) {
            server_logger->warn("Failed to accept a new TCP connection.");
            consecutive_failures++;

            if (consecutive_failures >= THRESHOLD_CONSECUTIVE_FAILURE) {
                server_logger->error("Too many ({}) consecutive accept() failures. Leave 1s interval", consecutive_failures);
                std::this_thread::sleep_for(std::chrono::seconds(1));
                consecutive_failures = 0;
            }
        }

        /* close right after acception */
        close(newSocketFileDescriptor);
    }
    // while (true) {
    //     uint64_t pingid = 0;
    //     std::string addr_msg_from;
    //     if (receive_udp_message(&ctx_server, pingid, addr_msg_from,
    //                         server_logger)) {
    //         // receive_message 실패 시 처리
    //         consecutive_failures++;
    //         server_logger->warn("receive_message failed ({} times in a row)",
    //                             consecutive_failures);

    //         // wait 1 second if 5 consecutive failures
    //         if (consecutive_failures >= THRESHOLD_CONSECUTIVE_FAILURE) {
    //             server_logger->error(
    //                 "Too many ({}) consecutive receive failures. Waiting 1 "
    //                 "second before retry...",
    //                 THRESHOLD_CONSECUTIVE_FAILURE);
    //             std::this_thread::sleep_for(std::chrono::seconds(1));
    //             consecutive_failures = 0;
    //         }

    //         continue;
    //     }

    //     if (send_udp_message(&ctx_server, addr_msg_from, PINGWEAVE_UDP_PORT_CLIENT,
    //                      pingid, server_logger)) {
    //         // somethign wrong
    //         server_logger->warn("Failed to send response to {}", addr_msg_from);
    //         continue;
    //     }
    // }
}