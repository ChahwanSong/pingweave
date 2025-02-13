#pragma once

#include "common.hpp"
#include "tcpudp_ping_info.hpp"

/*******************
 *       UDP       *
 ******************/
int make_ctx(struct udp_context *ctx, const std::string &ipv4,
             const uint16_t &port, std::shared_ptr<spdlog::logger> logger);

// Initialize TX/RX context for UDP
int initialize_contexts(struct udp_context &ctx_tx, udp_context &ctx_rx,
                        const std::string &ipv4, const uint16_t &rx_port,
                        std::shared_ptr<spdlog::logger> logger);

void log_bound_address(int sock_fd, std::shared_ptr<spdlog::logger> logger);

int send_udp_message(struct udp_context *ctx_tx, std::string dst_ip,
                     uint16_t dst_port, uint64_t pingid,
                     std::shared_ptr<spdlog::logger> logger);

int receive_udp_message(struct udp_context *ctx_rx, uint64_t &pingid,
                        std::string &sender_ip, uint64_t &steady_ts,
                        std::shared_ptr<spdlog::logger> logger);

int send_tcp_message(TcpUdpPinginfoMap *ping_table, std::string src_ip,
                     std::string dst_ip, uint16_t dst_port, uint64_t pingid,
                     std::shared_ptr<spdlog::logger> logger);

int receive_tcp_message(int sockfd, std::shared_ptr<spdlog::logger> logger);

/*******************
 *       TCP       *
 ******************/
int make_ctx(struct tcp_context *ctx, const std::string &ipv4,
             const uint16_t &port, std::shared_ptr<spdlog::logger> logger);

// statistics
std::string convert_tcpudp_result_to_str(
    const std::string &srcip, const std::string &dstip,
    const tcpudp_result_info_t &result_info, const result_stat_t &network_stat);