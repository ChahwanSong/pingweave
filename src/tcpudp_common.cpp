#include "tcpudp_common.hpp"

int make_ctx(struct udp_context *ctx, const std::string &ipv4,
             const uint16_t &port, std::shared_ptr<spdlog::logger> logger) {
    ctx->ipv4 = ipv4;

    // create socket
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) {
        logger->error("Failed to create UDP socket");
        return true;
    }
    ctx->sock = udp_socket(new int(fd));

    // bind socket
    sockaddr_in addr{};
    addr.sin_family = AF_INET;                 // IPV4
    addr.sin_addr.s_addr = htonl(INADDR_ANY);  // default
    addr.sin_port = htons(port);

    // change string IP to network byte order
    if (inet_pton(AF_INET, ipv4.data(), &addr.sin_addr) <= 0) {
        logger->error("Given ipv4 address is invalild: {}", ipv4);
        return true;
    }

    if (bind(*ctx->sock, reinterpret_cast<struct sockaddr *>(&addr),
             sizeof(addr)) < 0) {
        logger->error("Failed to bind UDP local addr");
        return true;
    }

    logger->info("IP: {}:{} is ready for UDP communication", ipv4, port);

    // success
    return false;
}

// Initialize TX/RX context for UDP
int initialize_contexts(struct udp_context &ctx_tx, struct udp_context &ctx_rx,
                        const std::string &ipv4,
                        std::shared_ptr<spdlog::logger> logger) {
    uint16_t tx_port = 0;
    uint16_t rx_port = PINGWEAVE_UDP_PORT_CLIENT;
    if (make_ctx(&ctx_tx, ipv4, tx_port, logger)) {
        logger->error("Failed to create TX context for IP: {}", ipv4);
        return true;
    }
    if (make_ctx(&ctx_rx, ipv4, rx_port, logger)) {
        logger->error("Failed to create RX context for IP: {}", ipv4);
        return true;
    }
    return false;
}

void log_bound_address(int sock_fd, std::shared_ptr<spdlog::logger> logger) {
    sockaddr_in bound_addr{};
    socklen_t addr_len = sizeof(bound_addr);
    if (getsockname(sock_fd, reinterpret_cast<struct sockaddr *>(&bound_addr),
                    &addr_len) == 0) {
        char ip_str[INET_ADDRSTRLEN];
        if (inet_ntop(AF_INET, &bound_addr.sin_addr, ip_str, sizeof(ip_str)) !=
            nullptr) {
            uint16_t port = ntohs(bound_addr.sin_port);
            logger->info("Server is bound to IP: {}, Port: {}", ip_str, port);
        } else {
            logger->warn("Failed to convert bound address to string");
        }
    } else {
        logger->warn("Failed to retrieve bound address info");
    }
}

int send_udp_message(struct udp_context *ctx_tx, const std::string &dst_ip,
                     const uint16_t &dst_port, const uint64_t &pingid,
                     std::shared_ptr<spdlog::logger> logger) {
    sockaddr_in dest{};
    dest.sin_family = AF_INET;
    dest.sin_port = htons(dst_port);

    if (inet_pton(AF_INET, dst_ip.data(), &dest.sin_addr) <= 0) {
        logger->error("Invalid host address: {}", dst_ip);
        return true;
    }

    union tcpudp_pingmsg_t msg;
    msg.x._prefix = 0;
    msg.x.pingid = pingid;
    msg.x._pad = 0;

    auto sent =
        sendto(*ctx_tx->sock, msg.raw, sizeof(tcpudp_pingmsg_t), 0,
               reinterpret_cast<struct sockaddr *>(&dest), sizeof(dest));
    if (sent < 0) {
        logger->error("Failed to send msg {} to {}", msg.x.pingid, dst_ip);
        return true;
    }

    if (static_cast<size_t>(sent) != sizeof(tcpudp_pingmsg_t)) {
        logger->error("Partial message sent");
        return true;
    }

    logger->debug("Sending UDP message with pingid {} to {}", pingid, dst_ip);

    // success
    return false;
}

int receive_udp_message(struct udp_context *ctx_rx, uint64_t &pingid,
                        std::string &sender,
                        std::shared_ptr<spdlog::logger> logger) {
    // clear a buffer
    memset(ctx_rx->buffer, 0, sizeof(tcpudp_pingmsg_t));
    union tcpudp_pingmsg_t ping_msg = {};

    // receive message
    sockaddr_in sender_addr{};
    socklen_t addr_len = sizeof(sender_addr);
    auto received =
        recvfrom(*ctx_rx->sock, ctx_rx->buffer, sizeof(tcpudp_pingmsg_t), 0,
                 reinterpret_cast<struct sockaddr *>(&sender_addr), &addr_len);

    // sanity check
    if (received < 0) {
        logger->error("Failed to receive UDP messsage");
        return true;  // error
    }

    // check received message size
    if (static_cast<size_t>(received) != sizeof(tcpudp_pingmsg_t)) {
        logger->error("Received unexpected message size: {} (expected: {})",
                      received, sizeof(tcpudp_pingmsg_t));
        return true;  // error
    }

    // parse the received message
    std::memcpy(&ping_msg, ctx_rx->buffer, sizeof(tcpudp_pingmsg_t));

    // sanity check
    if (ping_msg.x._prefix != 0 || ping_msg.x._pad != 0) {
        logger->error(
            "Prefix and pad is non-zero. Message might be corrupted.");
        return true;
    }
    if (ping_msg.x.pingid == 0) {
        logger->error("PingID must not be zero.");
        return true;
    }
    // memorize for PONG to sender
    pingid = ping_msg.x.pingid;

    // sender information
    char sender_ip[INET_ADDRSTRLEN];
    if (inet_ntop(AF_INET, &sender_addr.sin_addr, sender_ip,
                  sizeof(sender_ip)) == nullptr) {
        logger->error("Failed to parse a sender information from inet_ntop");
        return true;
    }
    sender = sender_ip;  // char to str
    // auto sender_port = ntohs(sender_addr.sin_port);

    logger->debug("Received UDP message with pingid {} from {}",
                  ping_msg.x.pingid, sender);

    // success
    return false;
}

int make_ctx(struct tcp_context *ctx, const std::string &ipv4,
             const uint16_t &port, std::shared_ptr<spdlog::logger> logger) {
    ctx->ipv4 = ipv4;

    // create socket
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        logger->error("Failed to create TCP socket");
        return true;
    }
    ctx->sock = tcp_socket(new int(fd));

    // set socket options
    int enable = 1;
    if (setsockopt(*ctx->sock, SOL_SOCKET, SO_REUSEADDR, &enable, sizeof(int)) <
        0) {
        logger->error("Faile dto set SO_REUSEADDR");
        return true;
    }
    if (setsockopt(*ctx->sock, SOL_SOCKET, SO_REUSEPORT, &enable, sizeof(int)) <
        0) {
        logger->error("Faile dto set SO_REUSEPORT");
        return true;
    }

    // set SO_LINGER:
    // (1) any queued data is thrown away and the reset is send immediately, and
    // (2) the receiver of the RST can tell that the other end did an abort
    // instead of a normal close
    struct linger solinger = {1, 0};
    if (setsockopt(*ctx->sock, SOL_SOCKET, SO_LINGER, &solinger, sizeof(struct linger)) == -1) {
        logger->error("Failed to set SO_LINGER");
        return true;
    }

    // bind socket
    sockaddr_in addr{};
    addr.sin_family = AF_INET;                   // IPV4
    addr.sin_addr.s_addr = ::htonl(INADDR_ANY);  // default
    addr.sin_port = htons(port);

    // change string IP to network byte order
    if (inet_pton(AF_INET, ipv4.data(), &addr.sin_addr) <= 0) {
        logger->error("Given ipv4 address is invalild: {}", ipv4);
        return true;
    }

    if (bind(*ctx->sock, reinterpret_cast<struct sockaddr *>(&addr),
               sizeof(addr)) < 0) {
        logger->error("Failed to bind TCP server local addr");
        return true;
    }

    // debugging - get the actual listening port number
    socklen_t addr_size = sizeof(addr);
    if (getsockname(*ctx->sock, reinterpret_cast<struct sockaddr *>(&addr), &addr_size) == -1) {
        logger->error("getsockname failed");
        return true;
    }
    logger->info("TCP server is lstening on port {}", ntohs(addr.sin_port));

    if (listen(*ctx->sock, SOMAXCONN) == -1) {
        logger->error("TCP server cannot listen the socket");
        return true;
    }

    // success
    return false;
}

std::string convert_tcpudp_result_to_str(
    const std::string &srcip, const std::string &dstip,
    const tcpudp_result_info_t &result_info,
    const result_stat_t &network_stat) {
    std::stringstream ss;
    ss << srcip << "," << dstip << ","
       << timestamp_ns_to_string(result_info.ts_start) << ","
       << timestamp_ns_to_string(result_info.ts_end) << ","
       << result_info.n_success << "," << result_info.n_failure << ","
       << result_info.n_weird << ","
       << "network," << network_stat.mean << "," << network_stat.max << ","
       << network_stat.percentile_50 << "," << network_stat.percentile_95 << ","
       << network_stat.percentile_99;

    // string 저장
    return ss.str();
}
