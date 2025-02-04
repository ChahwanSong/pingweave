#include <iostream>
#include <cstring>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>

#define PORT 50007
#define BUFFER_SIZE 1024

int main(int argc, char *argv[]) {
    const char* server_ip = (argc > 1) ? argv[1] : "0.0.0.0";

    int sockfd;
    struct sockaddr_in server_addr, client_addr;
    socklen_t client_addr_len = sizeof(client_addr);
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

    // 소켓 바인딩
    if (bind(sockfd, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
        perror("바인딩 실패");
        close(sockfd);
        return 1;
    }

    std::cout << "UDP 서버가 " << server_ip << ":" << PORT << " 에서 대기 중...\n";

    while (true) {
        ssize_t recv_len = recvfrom(sockfd, buffer, BUFFER_SIZE, 0,
                                    (struct sockaddr*)&client_addr, &client_addr_len);
        if (recv_len < 0) {
            perror("수신 실패");
            continue;
        }

        buffer[recv_len] = '\0';  // 문자열 종료
        // std::cout << "클라이언트 (" << inet_ntoa(client_addr.sin_addr) << ") 로부터 메시지 수신: " 
        //           << buffer << std::endl;

        // 받은 데이터를 클라이언트로 다시 전송 (에코)
        sendto(sockfd, buffer, recv_len, 0, (struct sockaddr*)&client_addr, client_addr_len);
    }

    close(sockfd);
    return 0;
}