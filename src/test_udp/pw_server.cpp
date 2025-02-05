#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstring>
#include <iostream>

#include "../common.hpp"
#include "../tcpudp_common.hpp"

// ./server 10.200.200.3 7227

int main(int argc, char *argv[]) {
    if (argc != 4) {
        std::cout << "Usage: " << argv[0]
                  << " <서버_IP> <서버_RX_PORT> <클라이언트_RX_PORT>\n"
                  << std::endl;
        return 1;
    }
    const char *server_ip = argv[1];
    const int server_rx_port = std::stoi(argv[2]);
    const int client_rx_port = std::stoi(argv[3]);

    spdlog::info("Start server on {}:{}", server_ip, server_rx_port);

    // Initialize UDP context
    udp_context ctx_server;
    if (make_ctx(&ctx_server, server_ip, server_rx_port,
                 spdlog::default_logger())) {
        spdlog::error("Failed to create TX context for IP: {}", server_ip);
        throw std::runtime_error("Failed to create UDP context at server");
    }

    while (true) {
        uint64_t pingid = 0;
        uint64_t dummy_ts;
        std::string addr_msg_from;
        if (receive_udp_message(&ctx_server, pingid, addr_msg_from, dummy_ts,
                                spdlog::default_logger())) {
            // receive_message 실패 시 처리
            spdlog::warn("receive_message failed");
            continue;
        }

        if (send_udp_message(&ctx_server, addr_msg_from, client_rx_port, pingid,
                             spdlog::default_logger())) {
            // somethign wrong
            spdlog::warn("Failed to send response to {}", addr_msg_from);
            continue;
        }

        spdlog::info("Server processed pingid {}", pingid);
    }
}