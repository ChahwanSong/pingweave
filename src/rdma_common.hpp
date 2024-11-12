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
#include <yaml-cpp/yaml.h>

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
#include "logger.hpp"

// fully-blocking SPSC queue
typedef moodycamel::BlockingReaderWriterQueue<struct result_msg_t>
    ClientInternalQueue;

enum {
    PINGWEAVE_WRID_RECV = 11,
    PINGWEAVE_WRID_SEND,
    PINGWEAVE_OPCODE_PONG,
    PINGWEAVE_OPCODE_ACK,
};

struct pingweave_context {
    struct ibv_context *context;
    struct ibv_comp_channel *channel;
    struct ibv_pd *pd;
    struct ibv_mr *mr;
    struct ibv_cq *cq;
    struct ibv_qp *qp;
    struct ibv_ah *ah;
    union {
        struct ibv_cq *cq;
        struct ibv_cq_ex *cq_ex;
    } cq_s;
    char *buf;
    int send_flags;
    int active_port;

    /* interface*/
    std::string ipv4;
    std::string iface;
    struct ibv_port_attr portinfo;
    int rnic_hw_ts;

    /* gid */
    union ibv_gid gid;
    char wired_gid[33];
    char parsed_gid[33];

    int is_rx;
    uint64_t completion_timestamp_mask;

    /* logging */
    std::string log_msg;
};

union rdma_addr {
    char raw[24];
    struct {
        uint32_t qpn;       // 4B
        union ibv_gid gid;  // 16B
        uint32_t lid;       // 4B
    } x;
};

// enum ibv_mtu pingweave_mtu_to_enum(int mtu);

void wire_gid_to_gid(const char *wgid, union ibv_gid *gid);
void gid_to_wire_gid(const union ibv_gid *gid, char wgid[]);
std::string parsed_gid(union ibv_gid *gid);

// Helper function to find RDMA device by matching network interface
int get_context_by_ifname(const char *ifname, struct pingweave_context *ctx);
int get_context_by_ip(struct pingweave_context *ctx);

// Find the active port from RNIC hardware
int find_active_port(struct pingweave_context *ctx);

struct ibv_cq *pingweave_cq(struct pingweave_context *ctx);

int init_ctx(struct pingweave_context *ctx);
int prepare_ctx(struct pingweave_context *ctx);
int make_ctx(struct pingweave_context *ctx, const std::string &ipv4,
             std::shared_ptr<spdlog::logger> logger, const int &is_rx);

int post_recv(struct pingweave_context *ctx, int n, const uint64_t &wr_id);
int post_send(struct pingweave_context *ctx, union rdma_addr rem_dest,
              const char *msg, const size_t &msg_len, const uint64_t &wr_id);

int save_device_info(struct pingweave_context *ctx,
                     std::shared_ptr<spdlog::logger> logger);
// for testing
int load_device_info(union rdma_addr *dst_addr,
                     std::shared_ptr<spdlog::logger> logger,
                     const std::string &filepath);

std::set<std::string> get_all_local_ips();

void get_my_addr(const std::string &filename, std::set<std::string> &myaddr);

// Utility function: Wait for CQ event and handle it
bool wait_for_cq_event(struct pingweave_context *ctx,
                       std::shared_ptr<spdlog::logger> logger);

std::string get_thread_id();

// statistics
result_stat_t calculateStatistics(const std::vector<uint64_t> &delays);

/**************************************************************/
/*************  I N L I N E   F U N C T I O N S  **************/
/**************************************************************/
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
    auto seconds = std::chrono::seconds(timestamp_ns / 1'000'000'000LL);
    auto nanoseconds_part = timestamp_ns % 1'000'000'000LL;

    // Convert to time_point
    std::chrono::time_point<std::chrono::system_clock> time_point(seconds);

    // Format to a human-readable string
    std::time_t time_t_format =
        std::chrono::system_clock::to_time_t(time_point);
    std::tm tm_format = *std::localtime(&time_t_format);

    std::ostringstream oss;
    oss << std::put_time(&tm_format, "%Y-%m-%d %H:%M:%S");
    oss << "." << std::setw(9) << std::setfill('0') << nanoseconds_part;

    return oss.str();
}

// Get current time as a formatted string
inline std::string get_current_timestamp_string() {
    // Get the current time point from the system clock
    auto now = std::chrono::system_clock::now();
    // Convert to time_t for formatting
    std::time_t now_time = std::chrono::system_clock::to_time_t(now);
    // Format the time as a string
    std::ostringstream oss;
    oss << std::put_time(std::localtime(&now_time), "%Y-%m-%d %H:%M:%S");
    return oss.str();
}

// Function to get current timestamp
inline uint64_t get_current_timestamp_steady() {
    struct timespec ts = {};
    if (clock_gettime(CLOCK_MONOTONIC, &ts) == -1) {
        throw std::runtime_error("Failed to call clock_gettime.");
    }
    return static_cast<uint64_t>(ts.tv_sec) * 1'000'000'000LL + ts.tv_nsec;
}

// IP 주소 문자열을 uint32_t로 변환하는 함수
inline uint32_t ip2uint(const std::string &ip) {
    uint32_t result;
    inet_pton(AF_INET, ip.c_str(), &result);
    return ntohl(result);  // 네트워크 바이트 순서에서 호스트 바이트 순서로 변환
}

// uint32_t를 IP 주소 문자열로 변환하는 함수
inline std::string uint2ip(uint32_t ip) {
    ip = htonl(ip);  // 호스트 바이트 순서를 네트워크 바이트 순서로 변환
    char buffer[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &ip, buffer, INET_ADDRSTRLEN);
    return std::string(buffer);
}
