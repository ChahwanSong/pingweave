#include <getopt.h>

#include <functional>
#include <stdexcept>

#include "rdma_client.hpp"
#include "rdma_server.hpp"
#include "udp_client.hpp"
#include "udp_server.hpp"

void print_usage() {
    std::cerr << "Usage: ./pingweave_simple -a <IPv4 address> -r|-u -s|-c\n"
              << "\nFlags:\n"
              << "  -a <IPv4 address>  Specify the IPv4 address (required).\n"
              << "  -r                 Use RDMA (optional but mutually "
                 "exclusive with -u).\n"
              << "  -u                 Use UDP (optional but mutually "
                 "exclusive with -r).\n"
              << "  -s                 Run as server (optional but mutually "
                 "exclusive with -c).\n"
              << "  -c                 Run as client (optional but mutually "
                 "exclusive with -s).\n";
}

int main(int argc, char** argv) {
    std::string ipv4_address;
    bool use_rdma = false;
    bool use_udp = false;
    bool is_server = false;
    bool is_client = false;

    int opt;
    while ((opt = getopt(argc, argv, "a:rusc")) != -1) {
        switch (opt) {
            case 'a':
                ipv4_address = optarg;
                break;
            case 'r':
                use_rdma = true;
                break;
            case 'u':
                use_udp = true;
                break;
            case 's':
                is_server = true;
                break;
            case 'c':
                is_client = true;
                break;
            default:
                print_usage();
                return 1;
        }
    }

    // Validate arguments
    if (ipv4_address.empty()) {
        std::cerr << "ERROR: IPv4 address (-a) is required.\n";
        print_usage();
        return 1;
    }

    if (use_rdma && use_udp) {
        std::cerr << "ERROR: Cannot use both RDMA (-r) and UDP (-u).\n";
        print_usage();
        return 1;
    }

    if (!use_rdma && !use_udp) {
        std::cerr << "ERROR: Either RDMA (-r) or UDP (-u) is required.\n";
        print_usage();
        return 1;
    }

    if (is_server && is_client) {
        std::cerr << "ERROR: Cannot run as both server (-s) and client (-c).\n";
        print_usage();
        return 1;
    }

    if (!is_server && !is_client) {
        std::cerr << "ERROR: Either server (-s) or client (-c) is required.\n";
        print_usage();
        return 1;
    }

    // Display configuration
    std::cout << "Configuration:\n";
    std::cout << "  IPv4 Address: " << ipv4_address << "\n";
    if (use_rdma) {
        std::cout << "  Mode: RDMA\n";
    } else if (use_udp) {
        std::cout << "  Mode: UDP\n";
    }
    if (is_server) {
        std::cout << "  Role: Server\n";
    } else if (is_client) {
        std::cout << "  Role: Client\n";
    }

    // Main program logic based on arguments
    if (is_server) {
        if (use_rdma) {
            std::cout << "Starting RDMA server...\n";
            rdma_server(ipv4_address);
        } else if (use_udp) {
            std::cout << "Starting UDP server...\n";
            udp_server(ipv4_address);
        }
    } else if (is_client) {
        if (use_rdma) {
            std::cout << "Starting RDMA client...\n";
            rdma_client(ipv4_address);
        } else if (use_udp) {
            std::cout << "Starting UDP client...\n";
            udp_client(ipv4_address);
        }
    }

    return 0;
}
