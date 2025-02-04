#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <cstring>
#include <iostream>

#include "../common.hpp"
#include "../tcpudp_common.hpp"

// ./client 10.200.200.3 10.200.200.2 7227

int main(int argc, char *argv[]) {
    if (argc < 4) {
        std::cerr << "Usage: " << argv[0]
                  << " <서버_IP> <클라이언트_IP> <서버_PORT> \n";
        return 1;
    }

    const char *server_ip = argv[1];
    const char *client_ip = argv[2];
    const int server_port = std::stoi(argv[3]);

    // Initialize UDP contexts
    udp_context ctx_tx, ctx_rx;
    if (initialize_contexts(ctx_tx, ctx_rx, client_ip,
                            spdlog::default_logger())) {
        throw std::runtime_error("Failed to initialize UDP contexts.");
    }

    // simple version
    uint64_t pingid = 0;
    int n = 0;
    double max_rtt = 0;
    while (true) {
        auto start_time = std::chrono::high_resolution_clock::now();

        // send
        if (send_udp_message(&ctx_tx, server_ip, server_port, ++pingid,
                             spdlog::default_logger())) {
            // somethign wrong
            spdlog::warn("Failed to send response to {}", server_ip);
            continue;
        }

        // recv
        std::string sender;
        uint64_t recv_time_steady;
        if (receive_udp_message(&ctx_rx, pingid, sender, recv_time_steady,
                                spdlog::default_logger())) {
            // receive_message 실패 시 처리
            spdlog::warn("receive_message failed");
            continue;
        }

        auto end_time = std::chrono::high_resolution_clock::now();
        std::chrono::duration<double, std::milli> rtt = end_time - start_time;

        n++;
        max_rtt = max_rtt > rtt.count() ? max_rtt : rtt.count();

        if (rtt.count() >= 1) {
            spdlog::info("RTT: {} ms", rtt.count());
        }

        if (n % 100 == 0 && n > 0) {
            spdlog::info("Max RTT: {} over {} trials...", max_rtt, n);
            max_rtt = 0;
            n = 0;
        }

        usleep(10000);  // 0.01초마다 반복
    }

    // int sockfd;
    // struct sockaddr_in server_addr;
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

    // int n = 0;
    // double max_rtt = 0;

    // try {
    //     while (true) {
    //         const char *message = "ping";
    //         auto start_time = std::chrono::high_resolution_clock::now();

    //         // 메시지 전송
    //         if (sendto(sockfd, message, strlen(message), 0,
    //                    (struct sockaddr*)&server_addr, sizeof(server_addr)) <
    //                    0) {
    //             perror("전송 실패");
    //             continue;
    //         }

    //         // 응답 수신
    //         socklen_t server_addr_len = sizeof(server_addr);
    //         ssize_t recv_len = recvfrom(sockfd, buffer, BUFFER_SIZE, 0,
    //                                     (struct sockaddr*)&server_addr,
    //                                     &server_addr_len);
    //         if (recv_len < 0) {
    //             perror("응답 수신 실패");
    //             continue;
    //         }

    //         auto end_time = std::chrono::high_resolution_clock::now();
    //         std::chrono::duration<double, std::milli> rtt = end_time -
    //         start_time;

    //         n++;
    //         max_rtt = max_rtt > rtt.count() ? max_rtt : rtt.count();

    //         buffer[recv_len] = '\0';  // 문자열 종료
    //         if (rtt.count() >= 1) {
    //             std::cout << "서버 응답: " << buffer << " | RTT: " <<
    //             rtt.count() << " ms" << std::endl;
    //         }

    //         if (n % 100 == 0 && n > 0) {
    //             std::cout << "Max RTT: " << max_rtt << ", Trial: " << n <<
    //             std::endl;

    //             max_rtt = 0;
    //             n = 0;
    //         }

    //         usleep(10000);  // 0.01초마다 반복
    //     }
    // } catch (...) {
    //     std::cerr << "예기치 않은 오류 발생\n";
    // }

    // close(sockfd);
    // return 0;
}