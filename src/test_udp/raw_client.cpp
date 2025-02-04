#include <iostream>
#include <cstring>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>
#include <chrono>

#define PORT 50007
#define BUFFER_SIZE 1024

int main(int argc, char *argv[]) {
    if (argc < 2) {
        std::cerr << "Usage: " << argv[0] << " <서버_IP>\n";
        return 1;
    }

    const char* server_ip = argv[1];

    int sockfd;
    struct sockaddr_in server_addr;
    char buffer[BUFFER_SIZE];

    // 소켓 생성
    if ((sockfd = socket(AF_INET, SOCK_DGRAM, 0)) < 0) {
        perror("소켓 생성 실패");
        return 1;
    }

    // 서버 주소 설정
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = inet_addr(server_ip);
    server_addr.sin_port = htons(PORT);

    int n = 0;
    double max_rtt = 0;

    try {
        while (true) {
            const char *message = "ping";
            auto start_time = std::chrono::high_resolution_clock::now();

            // 메시지 전송
            if (sendto(sockfd, message, strlen(message), 0,
                       (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
                perror("전송 실패");
                continue;
            }

            // 응답 수신
            socklen_t server_addr_len = sizeof(server_addr);
            ssize_t recv_len = recvfrom(sockfd, buffer, BUFFER_SIZE, 0,
                                        (struct sockaddr*)&server_addr, &server_addr_len);
            if (recv_len < 0) {
                perror("응답 수신 실패");
                continue;
            }

            auto end_time = std::chrono::high_resolution_clock::now();
            std::chrono::duration<double, std::milli> rtt = end_time - start_time;

            n++;
            max_rtt = max_rtt > rtt.count() ? max_rtt : rtt.count();

            buffer[recv_len] = '\0';  // 문자열 종료
            if (rtt.count() >= 1) {
                std::cout << "서버 응답: " << buffer << " | RTT: " << rtt.count() << " ms" << std::endl;
            }

            if (n % 100 == 0 && n > 0) {
                std::cout << "Max RTT: " << max_rtt << ", Trial: " << n << std::endl;
                
                max_rtt = 0;
                n = 0;
            } 

            usleep(10000);  // 0.01초마다 반복
        }
    } catch (...) {
        std::cerr << "예기치 않은 오류 발생\n";
    }

    close(sockfd);
    return 0;
}