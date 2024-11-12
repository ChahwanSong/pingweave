#pragma once

#include <infiniband/verbs.h>

#include <chrono>
#include <cstdint>

union ping_msg_t {
    char raw[36];
    struct {
        uint64_t pingid;    // 8B
        uint32_t qpn;       // 4B
        union ibv_gid gid;  // 16B
        uint64_t time;      // 8B
    } x;
};

union pong_msg_t {
    char raw[20];
    struct {
        uint32_t opcode;        // PONG or ACK
        uint64_t pingid;        // ping ID
        uint64_t server_delay;  // server's process delay
    } x;
};

// result msg of ping
struct alignas(64) result_msg_t {
    uint64_t pingid;
    uint32_t dstip;

    uint64_t time_ping_send;
    uint64_t client_delay;
    uint64_t network_delay;
    uint64_t server_delay;

    uint32_t success;  // 1: success, 0: failure
};

// result info of ping
struct result_info_t {
    uint32_t n_success = 0;
    uint32_t n_failure = 0;

    uint64_t ts_start = 0;
    uint64_t ts_end = 0;

    std::vector<uint64_t> server_delays;
    std::vector<uint64_t> network_delays;
    std::vector<uint64_t> client_delays;

    // default constructor
    result_info_t() = default;
};

// result stat
struct result_stat_t {
    uint64_t mean;
    uint64_t max;
    uint64_t percentile_50;
    uint64_t percentile_95;
    uint64_t percentile_99;
};