#include "tmp_udp_common.hpp"

int main(int argc, char* argv[]) {
    if (argc < 2) {
        std::fprintf(stderr, "Usage: %s [port]\n", argv[0]);
        return 1;
    }

    uint16_t port = static_cast<uint16_t>(std::stoi(argv[1]));

    auto receiver = create_udp_socket();
    bind_socket(receiver, port);

    std::printf("Waiting for message on port %u...\n", (unsigned)port);

    char buffer[1024];
    receive_message(receiver, buffer, sizeof(buffer));
    // std::printf("Received message: %s\n", buffer);

    return 0;
}