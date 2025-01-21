#pragma once

#include "tcpudp_common.hpp"
#include "tcpudp_ping_info.hpp"

/**
 * NOTE: TCP Server is simple. It receives SYN packet, and respond to it.
 */

void tcp_server_tx_thread(int sockfd, std::shared_ptr<spdlog::logger> logger) {
    // Instead of waiting FIN, use a timeout.
    // This is to avoid indefinite waiting and resource starvation. 
    std::this_thread::sleep_for(std::chrono::seconds(1));

    // // Wait the server-side FIN packet
    // char buffer[64];
    // ssize_t bytes_received;
    // while ((bytes_received = recv(sockfd, buffer, 64, 0)) > 0) {
    //     // In this scenario, the server does not send any data
    //     // So, this loop should exit when read returns 0 (connection closed)
    // }
    // if (bytes_received == 0) {
    //     logger->debug("Connection closed by server (FIN received)");
    // } else if (bytes_received < 0) {
    //     logger->error("recv loop failed");
    // }

    /* server makes passive-close (i.e., after FIN from client) */
    if (sockfd != -1) {
        close(sockfd);
    }
}

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
    ctx_server.is_server = true;
    if (make_ctx(&ctx_server, ipv4, PINGWEAVE_TCP_PORT_SERVER, server_logger)) {
        server_logger->error("Failed to create a server context for IP: {}",
                             ipv4);
        throw std::runtime_error("Failed to create TCP context at server");
    }

    // logging the address
    if (ctx_server.sock && *ctx_server.sock >= 0) {
        log_bound_address(*ctx_server.sock, server_logger);
    }

    sockaddr_in newSocketInfo;
    socklen_t newSocketInfoLength = sizeof(newSocketInfo);
    int newSockfd;

    while (true) {
        server_logger->debug("Waiting a new TCP connection...");
        newSockfd = accept(*ctx_server.sock, (sockaddr*)&newSocketInfo,
                           &newSocketInfoLength);
        if (newSockfd < 0) {
            server_logger->warn("Failed to accept a new TCP connection.");
            continue;
        }

        std::thread t(tcp_server_tx_thread, newSockfd, server_logger);
        t.detach();
    }
}