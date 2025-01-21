#pragma once

#include "tcpudp_common.hpp"
#include "tcpudp_ping_info.hpp"

/**
 * NOTE: TCP Server is simple. It receives SYN packet, and respond to it.
 */

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
        
        std::thread t(receive_tcp_message, newSockfd, server_logger);
        t.detach();
    }
}
