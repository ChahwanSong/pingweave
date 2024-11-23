#pragma once

#include <arpa/inet.h>
#include <dirent.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <infiniband/verbs.h>
#include <netdb.h>
#include <netinet/in.h>
#include <signal.h>  // For kill(), signal()
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>  // directory
#include <sys/types.h>
#include <sys/wait.h>  // For waitpid()
#include <time.h>
#include <unistd.h>  // For fork(), sleep()
#include <unistd.h>

#include <algorithm>
#include <atomic>  // std::atomic
#include <cerrno>  // errno
#include <chrono>
#include <cstdint>
#include <cstring>  // strerror
#include <filesystem>
#include <fstream>
#include <future>
#include <iostream>
#include <numeric>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <tuple>
#include <unordered_map>
#include <vector>

#include "format.hpp"
#include "ini.hpp"
#include "logger.hpp"
#include "macro.hpp"

// fully-blocking SPSC queue
typedef moodycamel::BlockingReaderWriterQueue<struct result_msg_t>
    ClientInternalQueue;
typedef moodycamel::ReaderWriterQueue<union ping_msg_t> ServerInternalQueue;

// Helper function to find RDMA device by matching network interface
int get_context_by_ifname(const char *ifname, struct pingweave_context *ctx);
int get_context_by_ip(struct pingweave_context *ctx);
// all ipv4 local address
std::set<std::string> get_all_local_ips();
void get_my_rdma_addr_from_pinglist(const std::string &pinglist_filename,
                                    std::set<std::string> &myaddr);
// Find the active port from RNIC hardware
int find_active_port(struct pingweave_context *ctx,
                     std::shared_ptr<spdlog::logger> logger);
int get_gid_table_size(struct pingweave_context *ctx,
                       std::shared_ptr<spdlog::logger> logger);
// parse ini file
int get_controller_info_from_ini(const std::string &ini_path, std::string &ip,
                                 int &port);
// delete files in directory
void delete_files_in_directory(const std::string &directoryPath);
// get thread ID
std::string get_thread_id();

struct ibv_cq *pingweave_cq(struct pingweave_context *ctx);
int init_ctx(struct pingweave_context *ctx);
int prepare_ctx(struct pingweave_context *ctx);
int make_ctx(struct pingweave_context *ctx, const std::string &ipv4,
             std::shared_ptr<spdlog::logger> logger, const int &is_rx);

// Initialize TX/RX context for RDMA UD
int initialize_contexts(pingweave_context &ctx_tx, pingweave_context &ctx_rx,
                        const std::string &ipv4,
                        std::shared_ptr<spdlog::logger> logger);

// RDMA Ops
int post_recv(struct pingweave_context *ctx, const uint64_t &wr_id,
              const int &n);
int post_send(struct pingweave_context *ctx, union rdma_addr rem_dest,
              const char *msg, const size_t &msg_len, const int &buf_idx,
              const uint64_t &wr_id, std::shared_ptr<spdlog::logger> logger);
int wait_for_cq_event(struct pingweave_context *ctx,
                      std::shared_ptr<spdlog::logger> logger);

int save_device_info(struct pingweave_context *ctx,
                     std::shared_ptr<spdlog::logger> logger);
int load_device_info(union rdma_addr *dst_addr, const std::string &filepath,
                     std::shared_ptr<spdlog::logger> logger);

// statistics
result_stat_t calc_stats(const std::vector<uint64_t> &delays);
// convert result to string
std::string convert_result_to_str(const std::string &srcip,
                                  const std::string &dstip,
                                  const result_info_t &result_info,
                                  const result_stat_t &client_stat,
                                  const result_stat_t &network_stat,
                                  const result_stat_t &server_stat);
// send the result to http server
void send_result_to_http_server(const std::string &server_ip, int server_port,
                                const std::string &message,
                                std::shared_ptr<spdlog::logger> logger);
/**************************************************************/
/*************  I N L I N E   F U N C T I O N S  **************/
/**************************************************************/

// use when writing to file
inline void wire_gid_to_gid(const char *wgid, union ibv_gid *gid) {
    char tmp[9];
    __be32 v32;
    int i;
    uint32_t tmp_gid[4];
    for (tmp[8] = 0, i = 0; i < 4; ++i) {
        memcpy(tmp, wgid + i * 8, 8);
        sscanf(tmp, "%x", &v32);
        tmp_gid[i] = be32toh(v32);
    }
    memcpy(gid, tmp_gid, sizeof(*gid));
}

// use when reading from file
inline void gid_to_wire_gid(const union ibv_gid *gid, char wgid[]) {
    uint32_t tmp_gid[4];
    int i;

    memcpy(tmp_gid, gid, sizeof(tmp_gid));
    for (i = 0; i < 4; ++i) {
        sprintf(&wgid[i * 8], "%08x", htobe32(tmp_gid[i]));
    }
}

// GID parser
inline std::string parsed_gid(union ibv_gid *gid) {
    char parsed_gid[33];
    inet_ntop(AF_INET6, gid, parsed_gid, sizeof(parsed_gid));
    return std::string(parsed_gid);
}

// Get current time in 64-bit nanoseconds
inline uint64_t get_current_timestamp_ns() {
    // Get the current time point from the system clock
    auto now = std::chrono::system_clock::now();
    // Convert to time since epoch in nanoseconds
    auto epoch_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                        now.time_since_epoch())
                        .count();
    return static_cast<uint64_t>(epoch_ns);
}

// Convert 64-bit nanoseconds timestamp to a human-readable string
inline std::string timestamp_ns_to_string(uint64_t timestamp_ns) {
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
inline std::string get_current_timestamp_string() {
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
inline uint64_t get_current_timestamp_steady() {
    struct timespec ts = {};
    if (clock_gettime(CLOCK_MONOTONIC, &ts) == -1) {
        throw std::runtime_error("Failed to call clock_gettime.");
    }
    return static_cast<uint64_t>(ts.tv_sec) * 1'000'000'000LL + ts.tv_nsec;
}

// Convert IP string to uint32_t (network byte order)
inline uint32_t ip2uint(const std::string &ip) {
    uint32_t result;
    inet_pton(AF_INET, ip.c_str(), &result);  // network byte order
    return result;
}

// Convert uint32_t (network byte order) to IP string
inline std::string uint2ip(const uint32_t &ip) {
    char buffer[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &ip, buffer, INET_ADDRSTRLEN);
    return std::string(buffer);
}

// two uint32_t (ip, uid) -> uint64_t
inline uint64_t make_pingid(const uint32_t &high, const uint32_t &low) {
    return (static_cast<uint64_t>(high) << 32) | low;
}

// uint64_t -> two uint32_t (ip, uid)
inline void parse_pingid(const uint64_t &value, uint32_t &high, uint32_t &low) {
    high = static_cast<uint32_t>(value >> 32);
    low = static_cast<uint32_t>(value & 0xFFFFFFFF);
}