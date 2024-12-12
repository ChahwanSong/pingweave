#pragma once

#include "common.hpp"

// fully-blocking SPSC queue
typedef moodycamel::BlockingReaderWriterQueue<struct udp_result_msg_t>
    UdpClientQueue;

int make_ctx(struct udp_context *ctx, const std::string &ipv4, const uint16_t &port,
             std::shared_ptr<spdlog::logger> logger);

// Initialize TX/RX context for UDP
int initialize_contexts(struct udp_context &ctx_tx, udp_context &ctx_rx,
                        const std::string &ipv4,
                        std::shared_ptr<spdlog::logger> logger);

void log_bound_address(int sock_fd, std::shared_ptr<spdlog::logger> logger);

int send_message(struct udp_context *ctx_tx, const std::string &dst_ip,
                 const uint16_t &dst_port, const uint64_t &pingid,
                 std::shared_ptr<spdlog::logger> logger);

int receive_message(struct udp_context *ctx_rx, uint64_t &pingid,
                    std::string &sender_ip,
                    std::shared_ptr<spdlog::logger> logger);

// statistics
std::string convert_udp_result_to_str(const std::string &srcip,
                                      const std::string &dstip,
                                      const udp_result_info_t &result_info,
                                      const result_stat_t &network_stat);