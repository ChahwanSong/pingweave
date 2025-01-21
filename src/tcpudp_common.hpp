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
                        const std::string &ipv4,
                        std::shared_ptr<spdlog::logger> logger);

void log_bound_address(int sock_fd, std::shared_ptr<spdlog::logger> logger);

int send_udp_message(struct udp_context *ctx_tx, const std::string &dst_ip,
                     const uint16_t &dst_port, const uint64_t &pingid,
                     std::shared_ptr<spdlog::logger> logger);

int receive_udp_message(struct udp_context *ctx_rx, uint64_t &pingid,
                        std::string &sender_ip,
                        std::shared_ptr<spdlog::logger> logger);

int send_tcp_message(TcpUdpPinginfoMap *ping_table, const std::string &src_ip,
                     const std::string &dst_ip, const uint16_t &dst_port,
                     const uint64_t &pingid,
                     std::shared_ptr<spdlog::logger> logger);

/*******************
 *       TCP       *
 ******************/
int make_ctx(struct tcp_context *ctx, const std::string &ipv4,
             const uint16_t &port, std::shared_ptr<spdlog::logger> logger);

// statistics
std::string convert_tcpudp_result_to_str(
    const std::string &srcip, const std::string &dstip,
    const tcpudp_result_info_t &result_info, const result_stat_t &network_stat);