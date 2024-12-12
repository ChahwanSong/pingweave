#include "common.hpp"

std::set<std::string> get_all_local_ips() {
    std::set<std::string> local_ips;
    struct ifaddrs *interfaces, *ifa;
    char ip[INET_ADDRSTRLEN];

    if (getifaddrs(&interfaces) == -1) {
        std::cerr << "Error getting interfaces." << std::endl;
        return local_ips;
    }

    for (ifa = interfaces; ifa != nullptr; ifa = ifa->ifa_next) {
        if (ifa->ifa_addr == nullptr) {
            continue;
        }

        std::string ifa_name = ifa->ifa_name;

        // Ignore interfaces starting with "virbr", "docker" or same as "lo"
        if (ifa_name == "lo" || ifa_name.rfind("virbr", 0) == 0 ||
            ifa_name.rfind("docker", 0) == 0) {
            continue;
        }

        if (ifa->ifa_addr->sa_family == AF_INET) {
            struct sockaddr_in *addr =
                reinterpret_cast<struct sockaddr_in *>(ifa->ifa_addr);
            inet_ntop(AF_INET, &(addr->sin_addr), ip, INET_ADDRSTRLEN);
            local_ips.insert(ip);
        }
    }

    freeifaddrs(interfaces);
    return local_ips;
}

// If error occurs, myaddr returned is empty set.
int get_my_addr_from_pinglist(const std::string &pinglist_filename,
                              std::set<std::string> &myaddr_rdma,
                              std::set<std::string> &myaddr_udp) {
    fkyaml::node config;
    myaddr_rdma.clear();
    myaddr_udp.clear(); 
    std::set<std::string> local_ips;

    try {
        std::ifstream ifs(pinglist_filename);
        config = fkyaml::node::deserialize(ifs);
        spdlog::debug("Pinglist.yaml loaded successfully.");
    } catch (const std::exception &e) {
        spdlog::error("Failed to load a pinglist.yaml - fkyaml:deserialize: {}",
                      e.what());
        return true;
    }

    // check empty or not
    if (config.empty()) {
        spdlog::warn("No entry in pinglist.yaml, skip.");
        return true;
    }

    try {
        // Retrieve the node's IP addr
        local_ips = get_all_local_ips();
    } catch (const std::exception &e) {
        spdlog::error("Failed to get my local IPs");
        return true;
    }

    try {        
        // Get the RDMA category groups
        if (!config.contains("rdma")) {
            spdlog::warn("No 'rdma' category found in pinglist.yaml");
        } else {
            // find all my ip addrs is in pinglist ip addrs
            for (auto &group : config["rdma"]) {
                for (auto &ip : group) {
                    // If IP is on the current node, add it
                    std::string ip_addr = ip.get_value_ref<std::string &>();
                    if (local_ips.find(ip_addr) != local_ips.end()) {
                        myaddr_rdma.insert(ip_addr);
                    }
                }
            }
        }

        // Get the UDP category groups
        if (!config.contains("udp")) {
            spdlog::warn("No 'udp' category found in pinglist.yaml");
        } else {
            // find all my ip addrs is in pinglist ip addrs
            for (auto &group : config["udp"]) {
                for (auto &ip : group) {
                    // If IP is on the current node, add it
                    std::string ip_addr = ip.get_value_ref<std::string &>();
                    if (local_ips.find(ip_addr) != local_ips.end()) {
                        myaddr_udp.insert(ip_addr);
                    }
                }
            }
        }
    } catch (const std::exception &e) {
        spdlog::error("Failure occurs while getting IP from pinglist: {}", e.what());
        return true;
    }

    // success
    return false;
}

int get_controller_info_from_ini(std::string &ip, int &port) {
    // path of pingweave.ini
    const std::string pingweave_ini_abs_path =
        get_source_directory() + DIR_CONFIG_PATH + "/pingweave.ini";

    IniParser parser;
    if (!parser.load(pingweave_ini_abs_path)) {
        spdlog::error("Failed to load pingweave.ini");
        return false;
    }

    ip = parser.get("controller", "host");
    if (ip.empty()) {
        spdlog::error("pingweave.ini gives an erratic controller host ip.");
        return false;
    }

    port = parser.getInt("controller", "port_collect");
    if (port < 0) {
        spdlog::error("pingweave.ini gives an erratic controller port.");
        return false;
    }

    // success
    return true;
}

int get_params_info_from_ini(int &val_1, int &val_2, int &val_3, int &val_4) {
    // path of pingweave.ini
    const std::string pingweave_ini_abs_path =
        get_source_directory() + DIR_CONFIG_PATH + "/pingweave.ini";

    IniParser parser;
    if (!parser.load(pingweave_ini_abs_path)) {
        spdlog::error("Failed to load pingweave.ini");
        return false;
    }

    val_1 = parser.getInt("param", "interval_sync_pinglist_sec");
    if (val_1 < 1) {
        spdlog::error(
            "pingweave.ini gives an erratic value for sync_pinglist_sec.");
        return false;
    }

    val_2 = parser.getInt("param", "interval_read_pinglist_sec");
    if (val_2 < 1) {
        spdlog::error(
            "pingweave.ini gives an erratic value for read_pinglist_sec.");
        return false;
    }

    val_3 = parser.getInt("param", "interval_report_ping_result_millisec");
    if (val_3 < 1) {
        spdlog::error(
            "pingweave.ini gives an erratic value for "
            "report_ping_result_millisec.");
        return false;
    }

    val_4 = parser.getInt("param", "interval_send_ping_microsec");
    if (val_4 < 1) {
        spdlog::error(
            "pingweave.ini gives an erratic value for send_ping_microsec.");
        return false;
    }

    // success
    return true;
}

void delete_files_in_directory(const std::string &directoryPath) {
    DIR *dir = opendir(directoryPath.c_str());
    if (!dir) {
        spdlog::error("Failed to open directory: {}", directoryPath);
        return;
    }

    struct dirent *entry;
    while ((entry = readdir(dir)) != nullptr) {
        if (strcmp(entry->d_name, ".") == 0 ||
            strcmp(entry->d_name, "..") == 0) {
            continue;
        }
        std::string filePath = directoryPath + "/" + entry->d_name;

        struct stat statbuf;
        if (stat(filePath.c_str(), &statbuf) == 0) {
            if (S_ISDIR(statbuf.st_mode)) {
                delete_files_in_directory(filePath);
                rmdir(filePath.c_str());
            } else {
                std::remove(filePath.c_str());
            }
            spdlog::debug("Deleted: {}", filePath);
        } else {
            spdlog::error("Failed to stat file: {}", filePath);
        }
    }
    closedir(dir);
}

// thread ID and cast to string
std::string get_thread_id() {
    std::stringstream ss;
    ss << std::this_thread::get_id();
    return ss.str();
}

// Convert IP string to uint32_t (network byte order)
uint32_t ip2uint(const std::string &ip) {
    uint32_t result;
    inet_pton(AF_INET, ip.c_str(), &result);  // network byte order
    return result;
}

// Convert uint32_t (network byte order) to IP string
std::string uint2ip(const uint32_t &ip) {
    char buffer[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &ip, buffer, INET_ADDRSTRLEN);
    return std::string(buffer);
}

// two uint32_t (ip, uid) -> uint64_t
uint64_t make_pingid(const uint32_t &high, const uint32_t &low) {
    return (static_cast<uint64_t>(high) << 32) | low;
}

// uint64_t -> two uint32_t (ip, uid)
void parse_pingid(const uint64_t &value, uint32_t &high, uint32_t &low) {
    high = static_cast<uint32_t>(value >> 32);
    low = static_cast<uint32_t>(value & 0xFFFFFFFF);
}

// Get current time in 64-bit nanoseconds
uint64_t get_current_timestamp_ns() {
    // Get the current time point from the system clock
    auto now = std::chrono::system_clock::now();
    // Convert to time since epoch in nanoseconds
    auto epoch_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                        now.time_since_epoch())
                        .count();
    return static_cast<uint64_t>(epoch_ns);
}

// Convert 64-bit nanoseconds timestamp to a human-readable string
std::string timestamp_ns_to_string(uint64_t timestamp_ns) {
    // Convert nanoseconds to seconds and nanoseconds part
    auto seconds = timestamp_ns / 1'000'000'000LL;
    auto nanoseconds_part = timestamp_ns % 1'000'000'000LL;

    // Convert to time_t and tm structure
    std::time_t time_t_format = static_cast<std::time_t>(seconds);
    std::tm tm_format = *std::localtime(&time_t_format);

    // Format time to a human-readable string
    char time_buffer[32];
    std::strftime(time_buffer, sizeof(time_buffer), "%Y-%m-%d %H:%M:%S",
                  &tm_format);

    // Combine formatted time and nanoseconds
    char result_buffer[64];
    std::snprintf(result_buffer, sizeof(result_buffer), "%s.%09llu",
                  time_buffer, nanoseconds_part);

    return std::string(result_buffer);
}

// Get current time as a formatted string
std::string get_current_timestamp_string() {
    // Get the current time point from the system clock
    auto now = std::chrono::system_clock::now();
    // Convert to time_t for formatting
    std::time_t now_time = std::chrono::system_clock::to_time_t(now);
    // Convert to tm structure for local time
    std::tm tm_format = *std::localtime(&now_time);

    // Format the time as a string
    char time_buffer[32];
    std::strftime(time_buffer, sizeof(time_buffer), "%Y-%m-%d %H:%M:%S",
                  &tm_format);

    return std::string(time_buffer);
}
// Function to get current timestamp
uint64_t get_current_timestamp_steady() {
    struct timespec ts = {};
    if (clock_gettime(CLOCK_MONOTONIC, &ts) == -1) {
        throw std::runtime_error("Failed to call clock_gettime.");
    }
    return static_cast<uint64_t>(ts.tv_sec) * 1'000'000'000LL + ts.tv_nsec;
}

/**
 * req_api: /result_rdma, /alarm, etc
 */
void send_message_to_http_server(const std::string &server_ip, int server_port,
                                 const std::string &message,
                                 const std::string &req_api,
                                 std::shared_ptr<spdlog::logger> logger) {
    // create socket
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        logger->error("HTTP Socket creation failed! errno: {} - {}", errno,
                      strerror(errno));
        return;
    }

    // set timeout (3 seconds, by default)
    const int timeout_sec = 3;
    timeval timeout{};
    timeout.tv_sec = timeout_sec;
    timeout.tv_usec = 0;
    if (setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) <
        0) {
        logger->error("HTTP Failed to set timeout! errno: {} - {}", errno,
                      strerror(errno));
        close(sock);
        return;
    }

    // set http server address
    sockaddr_in server_addr{};
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(server_port);
    if (inet_pton(AF_INET, server_ip.c_str(), &server_addr.sin_addr) <= 0) {
        logger->error("HTTP Invalid server IP address: {}! errno: {} - {}",
                      server_ip, errno, strerror(errno));
        close(sock);
        return;
    }

    // connect to server
    if (connect(sock, (struct sockaddr *)&server_addr, sizeof(server_addr)) <
        0) {
        logger->error("HTTP Connection to server {}:{} failed! errno: {} - {}",
                      server_ip, server_port, errno, strerror(errno));
        close(sock);
        return;
    }

    // construct HTTP request
    std::string request = "POST " + req_api +
                          " HTTP/1.1\r\n"
                          "Host: " +
                          server_ip + ":" + std::to_string(server_port) +
                          "\r\n"
                          "Content-Type: text/plain\r\n"
                          "Content-Length: " +
                          std::to_string(message.size()) + "\r\n\r\n" + message;

    // send request
    ssize_t bytes_sent = send(sock, request.c_str(), request.size(), 0);
    if (bytes_sent < 0) {
        logger->error("HTTP Failed to send request to {}:{}! errno: {} - {}",
                      server_ip, server_port, errno, strerror(errno));
        close(sock);
        return;
    } else if (bytes_sent < static_cast<ssize_t>(request.size())) {
        logger->warn("HTTP Partial send: Only {}/{} bytes sent to {}:{}!",
                     bytes_sent, request.size(), server_ip, server_port);
    }

    // Close the socket
    if (close(sock) < 0) {
        logger->error("HTTP Socket close failed! errno: {} - {}", errno,
                      strerror(errno));
    }
}

int message_to_http_server(const std::string &message, const std::string &api,
                           std::shared_ptr<spdlog::logger> logger) {
    const std::string pingweave_ini_abs_path =
        get_source_directory() + DIR_CONFIG_PATH + "/pingweave_server.py";

    std::string controller_host;
    int controller_port;
    if (get_controller_info_from_ini(controller_host, controller_port)) {
        send_message_to_http_server(controller_host, controller_port, message,
                                    api, logger);
        return false;  // success
    }

    // failed
    logger->error("Failed to post /result_rdma.");
    return true;
}

// calculate stats from delay histroy
result_stat_t calc_result_stats(const std::vector<uint64_t> &delays) {
    if (delays.empty()) {
        // 벡터가 비어 있을 때 -1 반환
        return {static_cast<uint64_t>(0), static_cast<uint64_t>(0),
                static_cast<uint64_t>(0), static_cast<uint64_t>(0),
                static_cast<uint64_t>(0)};
    }

    // mean
    uint64_t sum = std::accumulate(delays.begin(), delays.end(), uint64_t(0));
    uint64_t mean = sum / delays.size();

    // max
    uint64_t max = *std::max_element(delays.begin(), delays.end());

    // sort vector for percentile
    std::vector<uint64_t> sorted_delays = delays;
    std::sort(sorted_delays.begin(), sorted_delays.end());

    // median, 95-percentile, 99-percentile index
    uint64_t percentile_50 = sorted_delays[sorted_delays.size() * 50 / 100];
    uint64_t percentile_95 = sorted_delays[sorted_delays.size() * 95 / 100];
    uint64_t percentile_99 = sorted_delays[sorted_delays.size() * 99 / 100];

    return {mean, max, percentile_50, percentile_95, percentile_99};
}
