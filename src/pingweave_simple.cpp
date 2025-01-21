#include "rdma_client.hpp"
#include "rdma_server.hpp"
#include "tcp_client.hpp"
#include "tcp_server.hpp"
#include "udp_client.hpp"
#include "udp_server.hpp"

void print_usage() {
    std::cerr
        << "Usage: ./pingweave_simple -a <IPv4 address> "
           "--roce|--ib|--udp|--tcp -s|-c\n"
        << "\nFlags:\n"
        << "  -a <IPv4 address>  Specify the IPv4 address (required).\n"
        << "  --roce             Use RoCEv2 (optional but mutually "
           "exclusive with -udp, -ib, and -tcp).\n"
        << "  --ib               Use Infiniband (optional but mutually "
           "exclusive with -roce, -udp, and -tcp).\n"
        << "  --udp              Use UDP (optional but mutually exclusive "
           "with -roce, -ib, and -tcp).\n"
        << "  --tcp              Use TCP (optional but mutually exclusive "
           "with -roce, -ib, and -udp).\n"
        << "  -s                Run as server (optional but mutually "
           "exclusive with -c).\n"
        << "  -c                Run as client (optional but mutually "
           "exclusive with -s).\n";
}

void set_process_name(const std::string& new_name) {
    // `program_invocation_name` points to the original `argv[0]`
    extern char* program_invocation_name;
    strncpy(program_invocation_name, new_name.c_str(),
            strlen(program_invocation_name));
    program_invocation_name[strlen(new_name.c_str())] = '\0';
}

// Function to start a new process running the given function
pid_t start_process(std::function<void()> func, const std::string& name) {
    pid_t pid = fork();
    if (pid < 0) {
        spdlog::error("Failed to fork process for {}: {}", name,
                      strerror(errno));
        exit(EXIT_FAILURE);
    } else if (pid == 0) {
        // Child process: reset signal handlers to default
        signal(SIGINT, SIG_DFL);
        signal(SIGTERM, SIG_DFL);
        signal(SIGCHLD, SIG_DFL);  // Reset SIGCHLD if necessary

        // Set the process name for child
        set_process_name(name);

        // Child process: execute the function
        spdlog::info("-> {} - Child Process (PID: {}) started.", name,
                     getpid());
        func();
        exit(EXIT_SUCCESS);  // Should not reach here
    } else {
        // Parent process: return child's PID
        return pid;
    }
}

int main(int argc, char** argv) {
    std::string ipv4_address;
    bool use_roce = false;
    bool use_ib = false;
    bool use_udp = false;
    bool use_tcp = false;
    bool is_server = false;
    bool is_client = false;

    static struct option long_options[] = {{"roce", no_argument, nullptr, 1},
                                           {"ib", no_argument, nullptr, 2},
                                           {"udp", no_argument, nullptr, 3},
                                           {"tcp", no_argument, nullptr, 4},
                                           {0, 0, 0, 0}};

    int opt;
    while ((opt = getopt_long(argc, argv, "a:sc", long_options, nullptr)) !=
           -1) {
        switch (opt) {
            case 'a':
                ipv4_address = optarg;
                break;
            case 1:
                use_roce = true;
                break;
            case 2:
                use_ib = true;
                break;
            case 3:
                use_udp = true;
                break;
            case 4:
                use_tcp = true;
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

    if ((use_roce + use_ib + use_udp + use_tcp) > 1) {
        std::cerr << "ERROR: Cannot use multiple modes (--roce, --ib, --udp, "
                     "--tcp) simultaneously.\n";
        print_usage();
        return 1;
    }

    if (!use_roce && !use_ib && !use_udp && !use_tcp) {
        std::cerr
            << "ERROR: One mode (--roce, --ib, --udp, --tcp) is required.\n";
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
    if (use_roce) {
        std::cout << "  Mode: RoCEv2\n";
    } else if (use_ib) {
        std::cout << "  Mode: Infiniband\n";
    } else if (use_udp) {
        std::cout << "  Mode: UDP\n";
    } else if (use_tcp) {
        std::cout << "  Mode: TCP\n";
    }
    if (is_server) {
        std::cout << "  Role: Server\n";
    } else if (is_client) {
        std::cout << "  Role: Client\n";
    }

    // Main program logic based on arguments
    if (is_server) {
        // run pingweave_server.py
        std::string py_server_path =
            get_src_dir() + "/" + "pingweave_server.py";
        start_process(
            [&] {
                execlp("python3", "python3", py_server_path.c_str(),
                       (char*)nullptr);
            },
            "py_server");

        if (use_roce) {
            std::cout << "Starting RoCEv2 server...\n";
            rdma_server(ipv4_address, "roce");
        } else if (use_ib) {
            std::cout << "Starting Infiniband server...\n";
            rdma_server(ipv4_address, "ib");
        } else if (use_udp) {
            std::cout << "Starting UDP server...\n";
            udp_server(ipv4_address, "udp");
        } else if (use_tcp) {
            std::cout << "Starting TCP server...\n";
            tcp_server(ipv4_address, "tcp");
        }
    } else if (is_client) {
        // run pingweave_client.py
        std::string py_client_path =
            get_src_dir() + "/" + "pingweave_client.py";
        start_process(
            [&] {
                execlp("python3", "python3", py_client_path.c_str(),
                       (char*)nullptr);
            },
            "py_client");

        if (use_roce) {
            std::cout << "Starting RoCEv2 client...\n";
            rdma_client(ipv4_address, "roce");
        } else if (use_ib) {
            std::cout << "Starting Infiniband client...\n";
            rdma_client(ipv4_address, "ib");
        } else if (use_udp) {
            std::cout << "Starting UDP client...\n";
            udp_client(ipv4_address, "udp");
        } else if (use_tcp) {
            std::cout << "Starting TCP client...\n";
            tcp_client(ipv4_address, "tcp");
        }
    }

    return 0;
}