#include "tmp_udp_common.hpp"

int main(int argc, char* argv[]) {
    if (argc < 4) {
        std::fprintf(stderr, "Usage: %s [host] [port] [message]\n", argv[0]);
        return 1;
    }

    std::string host{argv[1]};
    uint16_t port = static_cast<uint16_t>(std::stoi(argv[2]));
    std::string msg{argv[3]};

    auto sender = create_udp_socket();
    send_message(sender, host, port, msg);
    std::printf("Message sent to %.*s:%u\n", static_cast<int>(host.size()), host.data(), (unsigned)port);

    return 0;
}