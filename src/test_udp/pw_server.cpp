#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstring>
#include <iostream>

#include "../common.hpp"
#include "../tcpudp_common.hpp"

// ./server 10.200.200.3 7227

// int receive_udp_message(struct udp_context *ctx_rx, uint64_t &pingid,
//                         std::string &sender, uint64_t &steady_ts,
//                         std::shared_ptr<spdlog::logger> logger) {
//     // clear a buffer
//     memset(ctx_rx->buffer, 0, sizeof(tcpudp_pingmsg_t));
//     union tcpudp_pingmsg_t ping_msg = {};

//     // receive message
//     sockaddr_in sender_addr{};
//     socklen_t addr_len = sizeof(sender_addr);
//     int received = -1;

//     if (ctx_rx->poll_interval_us > 0) {
//         // non-blocking
//         while (true) {
//             received = recvfrom(
//                 *ctx_rx->sock, ctx_rx->buffer, sizeof(tcpudp_pingmsg_t), 0,
//                 reinterpret_cast<struct sockaddr *>(&sender_addr),
//                 &addr_len);
//             if (received < 0) {
//                 if (errno == EAGAIN || errno == EWOULDBLOCK) {
//                     // retry after a short interval
//                     std::this_thread::sleep_for(
//                         std::chrono::microseconds(ctx_rx->poll_interval_us));
//                     continue;
//                 } else {
//                     logger->error("recvfrom() failed: {}", strerror(errno));
//                     return true;
//                 }
//             }
//             // successful, exit the loop
//             break;
//         }
//     } else {
//         received = recvfrom(
//             *ctx_rx->sock, ctx_rx->buffer, sizeof(tcpudp_pingmsg_t), 0,
//             reinterpret_cast<struct sockaddr *>(&sender_addr), &addr_len);
//     }

//     // get recv time
//     steady_ts = get_current_timestamp_steady_ns();

//     // sanity check
//     if (received < 0) {
//         logger->error("Failed to receive UDP messsage");
//         return true;  // error
//     }

//     // check received message size
//     if (static_cast<size_t>(received) != sizeof(tcpudp_pingmsg_t)) {
//         logger->error("Received unexpected message size: {} (expected: {})",
//                       received, sizeof(tcpudp_pingmsg_t));
//         return true;  // error
//     }

//     // parse the received message
//     std::memcpy(&ping_msg, ctx_rx->buffer, sizeof(tcpudp_pingmsg_t));

//     // sanity check
//     if (ping_msg.x._prefix != 0 || ping_msg.x._pad != 0) {
//         logger->error(
//             "Prefix and pad is non-zero. Message might be corrupted.");
//         return true;
//     }
//     if (ping_msg.x.pingid == 0) {
//         logger->error("PingID must not be zero.");
//         return true;
//     }

//     // memorize for PONG to sender
//     pingid = ping_msg.x.pingid;

//     // sender information
//     char sender_ip[INET_ADDRSTRLEN];
//     if (inet_ntop(AF_INET, &sender_addr.sin_addr, sender_ip,
//                   sizeof(sender_ip)) == nullptr) {
//         logger->error("Failed to parse a sender information from inet_ntop");
//         return true;
//     }
//     sender = sender_ip;  // char to str
//     // auto sender_port = ntohs(sender_addr.sin_port);

//     logger->debug("Received UDP message with pingid {} from {}",
//                   ping_msg.x.pingid, sender);

//     // success
//     return false;
// }

// int send_udp_message(struct udp_context *ctx_tx, std::string dst_ip,
//                      uint16_t dst_port, uint64_t pingid,
//                      std::shared_ptr<spdlog::logger> logger) {
//     sockaddr_in dest{};
//     dest.sin_family = AF_INET;
//     dest.sin_port = htons(dst_port);

//     if (inet_pton(AF_INET, dst_ip.data(), &dest.sin_addr) <= 0) {
//         logger->error("Invalid host address: {}", dst_ip);
//         return true;
//     }

//     union tcpudp_pingmsg_t msg;
//     msg.x._prefix = 0;
//     msg.x.pingid = pingid;
//     msg.x._pad = 0;

//     auto sent =
//         sendto(*ctx_tx->sock, msg.raw, sizeof(tcpudp_pingmsg_t), 0,
//                reinterpret_cast<struct sockaddr *>(&dest), sizeof(dest));

//     if (sent < 0) {
//         logger->error("Failed to send msg {} to {}", msg.x.pingid, dst_ip);
//         return true;
//     }

//     if (static_cast<size_t>(sent) != sizeof(tcpudp_pingmsg_t)) {
//         logger->error("Partial message sent");
//         return true;
//     }

//     logger->debug("Sending UDP message with pingid {} to {}", pingid,
//     dst_ip);

//     // success
//     return false;
// }

int main(int argc, char *argv[]) {
    const char *server_ip = (argc > 1) ? argv[1] : "0.0.0.0";
    const int server_port =
        (argc > 2) ? std::stoi(argv[2]) : PINGWEAVE_UDP_PORT_CLIENT;

    spdlog::info("Start server on {}:{}", server_ip, server_port);

    // Initialize UDP context
    udp_context ctx_server;
    if (make_ctx(&ctx_server, server_ip, server_port,
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

        if (send_udp_message(&ctx_server, addr_msg_from,
                             PINGWEAVE_UDP_PORT_CLIENT, pingid,
                             spdlog::default_logger())) {
            // somethign wrong
            spdlog::warn("Failed to send response to {}", addr_msg_from);
            continue;
        }

        spdlog::info("Server processed pingid {}", pingid);
    }

    // int sockfd;
    // struct sockaddr_in server_addr, client_addr;
    // socklen_t client_addr_len = sizeof(client_addr);
    // char buffer[BUFFER_SIZE];

    // // 소켓 생성
    // if ((sockfd = socket(AF_INET, SOCK_DGRAM, 0)) < 0) {
    //     perror("소켓 생성 실패");
    //     return 1;
    // }

    // // 서버 주소 설정
    // memset(&server_addr, 0, sizeof(server_addr));
    // server_addr.sin_family = AF_INET;
    // server_addr.sin_addr.s_addr = inet_addr(server_ip);
    // server_addr.sin_port = htons(PORT);

    // // 소켓 바인딩
    // if (bind(sockfd, (struct sockaddr*)&server_addr, sizeof(server_addr)) <
    // 0) {
    //     perror("바인딩 실패");
    //     close(sockfd);
    //     return 1;
    // }

    // std::cout << "UDP 서버가 " << server_ip << ":" << PORT << " 에서 대기
    // 중...\n";

    // while (true) {
    //     ssize_t recv_len = recvfrom(sockfd, buffer, BUFFER_SIZE, 0,
    //                                 (struct sockaddr*)&client_addr,
    //                                 &client_addr_len);
    //     if (recv_len < 0) {
    //         perror("수신 실패");
    //         continue;
    //     }

    //     buffer[recv_len] = '\0';  // 문자열 종료
    //     // std::cout << "클라이언트 (" << inet_ntoa(client_addr.sin_addr) <<
    //     ") 로부터 메시지 수신: "
    //     //           << buffer << std::endl;

    //     // 받은 데이터를 클라이언트로 다시 전송 (에코)
    //     sendto(sockfd, buffer, recv_len, 0, (struct sockaddr*)&client_addr,
    //     client_addr_len);
    // }

    // close(sockfd);
    // return 0;
}