#include "tmp_udp_common.hpp"

#include <arpa/inet.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

void close_fd::operator()(int* p) const noexcept {
    if (p && *p >= 0) {
        ::close(*p);
    }
    delete p;
}

udp_socket create_udp_socket() {
    int fd = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) {
        std::perror("socket");
        std::exit(1);
    }
    return udp_socket(new int(fd));
}

void bind_socket(const udp_socket& sock, uint16_t port) {
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = ::htonl(INADDR_ANY);
    addr.sin_port = ::htons(port);

    if (::bind(*sock, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) <
        0) {
        std::perror("bind");
        std::exit(1);
    }
}

void send_message(const udp_socket& sock, std::string host, uint16_t port,
                  std::string msg) {
    sockaddr_in dest{};
    dest.sin_family = AF_INET;
    dest.sin_port = ::htons(port);

    if (::inet_pton(AF_INET, host.data(), &dest.sin_addr) <= 0) {
        std::fprintf(stderr, "Invalid host address: %.*s\n",
                     static_cast<int>(host.size()), host.data());
        std::exit(1);
    }

    ssize_t sent =
        ::sendto(*sock, msg.data(), msg.size(), 0,
                 reinterpret_cast<struct sockaddr*>(&dest), sizeof(dest));
    if (sent < 0) {
        std::perror("sendto");
        std::exit(1);
    }

    if (static_cast<size_t>(sent) != msg.size()) {
        std::fprintf(stderr, "Partial message sent\n");
        std::exit(1);
    }
}

ssize_t receive_message(const udp_socket& sock, char* buffer, size_t bufsize) {
    sockaddr_in sender_addr{};
    socklen_t addr_len = sizeof(sender_addr);
    ssize_t received =
        ::recvfrom(*sock, buffer, bufsize, 0,
                   reinterpret_cast<struct sockaddr*>(&sender_addr), &addr_len);

    if (received < 0) {
        std::perror("recvfrom");
        std::exit(1);
    }

    // 널 종결자 처리
    if (static_cast<size_t>(received) < bufsize) {
        buffer[received] = '\0';
    } else {
        buffer[bufsize - 1] = '\0';
    }

    // 송신자 주소 정보 출력
    char sender_ip[INET_ADDRSTRLEN];
    if (inet_ntop(AF_INET, &sender_addr.sin_addr, sender_ip,
                  sizeof(sender_ip)) == nullptr) {
        perror("inet_ntop");
        exit(1);
    }

    unsigned short sender_port = ntohs(sender_addr.sin_port);
    printf("Received message: '%s'\n", buffer);
    printf("From: %s:%hu\n", sender_ip, sender_port);

    return received;
}