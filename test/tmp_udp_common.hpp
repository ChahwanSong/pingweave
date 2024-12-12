#pragma once

#include <string>
#include <memory>
#include <cstdint>
#include <sys/types.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <arpa/inet.h>
#include <unistd.h>

struct close_fd {
    void operator()(int* p) const noexcept;
};

using udp_socket = std::unique_ptr<int, close_fd>;

udp_socket create_udp_socket();
void bind_socket(const udp_socket& sock, uint16_t port);
void send_message(const udp_socket& sock, std::string host, uint16_t port, std::string msg);
ssize_t receive_message(const udp_socket& sock, char* buffer, size_t bufsize);
