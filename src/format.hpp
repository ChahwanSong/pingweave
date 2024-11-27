#pragma once

#include <infiniband/verbs.h>

#include <chrono>
#include <cstdint>

#include "macro.hpp"

// pingweave enumerate
enum {
    PINGWEAVE_OPCODE_PONG = 1,
    PINGWEAVE_OPCODE_ACK = 2,
    PINGWEAVE_WRID_PONG_ACK = (NUM_BUFFER * 4),  // to ignore
};

// ping message with client's information
union ping_msg_t {
    char raw[40];
    struct {
        uint64_t pingid;  // IP ++ PingUID
        uint32_t qpn;
        union ibv_gid gid;
        uint32_t lid;
        // time field is used in ping_table
        uint64_t time;  // arrival time at server
    } x;
};

// server's response message (pong)
union pong_msg_t {
    char raw[20];
    struct {
        uint32_t opcode;        // PONG or ACK
        uint64_t pingid;        // ping ID
        uint64_t server_delay;  // server's process delay (for ACK)
    } x;
};

// result msg of ping
struct alignas(64) result_msg_t {
    uint64_t pingid;
    uint32_t dstip;

    uint64_t time_ping_send;  // timestamp of ping send
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

struct Buffer {
    char *addr;         // Pointer to the buffer
    size_t length;      // Size of the buffer
    struct ibv_mr *mr;  // Memory region after registration
};

struct pingweave_context {
    struct ibv_context *context;
    struct ibv_comp_channel *channel;
    struct ibv_pd *pd;
    struct ibv_qp *qp;
    union {
        struct ibv_cq *cq;
        struct ibv_cq_ex *cq_ex;
    } cq_s;

    /* buffer */
    std::vector<Buffer> buf;

    /* interface*/
    int gid_index;
    std::string ipv4;
    std::string iface;
    struct ibv_port_attr portinfo;
    int rnic_hw_ts;
    int send_flags;
    int active_port;
    int is_rx;
    uint64_t completion_timestamp_mask;

    /* gid */
    union ibv_gid gid;
    char wired_gid[33];
    char parsed_gid[33];
};

union rdma_addr {
    char raw[24];
    struct {
        uint32_t qpn;       // 4B
        union ibv_gid gid;  // 16B
        uint32_t lid;       // 4B
    } x;
};