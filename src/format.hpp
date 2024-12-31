#pragma once

#include <infiniband/verbs.h>

#include <chrono>
#include <cstdint>
#include <memory>  // std::unique_ptr

#include "macro.hpp"

// pingweave enumerate
enum {
    // WEIRD: when ping-pong (network) delay at client-side is 
    // lower than server-side process delay.
    PINGWEAVE_RESULT_WEIRD = -2, 
    PINGWEAVE_RESULT_FAILURE = -1,
    PINGWEAVE_RESULT_SUCCESS = 0,
    
    // OPCODE, WR_ID for RDMA
    PINGWEAVE_OPCODE_PONG = 1,
    PINGWEAVE_OPCODE_ACK = 2,
    PINGWEAVE_WRID_PONG_ACK = (NUM_BUFFER * 4),  // to ignore
};



// result stat
struct result_stat_t {
    uint64_t mean;
    uint64_t max;
    uint64_t percentile_50;
    uint64_t percentile_95;
    uint64_t percentile_99;
};

/*******
 * UDP *
 ********/
union udp_pingmsg_t {
    char raw[16];
    struct {
        uint32_t _prefix; // IP ++ PingUID
        uint64_t pingid;
        uint32_t _pad;
    } x;
};

struct close_fd {
    void operator()(int *p) const noexcept {
        if (p && *p >= 0) {
            ::close(*p);
        }
        delete p;
    }
};

// UDP socket
using udp_socket = std::unique_ptr<int, close_fd>;

// context for UDP ping
struct udp_context {
    /* socket */
    udp_socket sock;

    /* buffer */
    char buffer[sizeof(udp_pingmsg_t)];

    /* interface*/
    std::string ipv4;
    std::string iface;
};

// udp ping result msg
struct alignas(32) udp_result_msg_t {
    uint64_t pingid;
    uint32_t dstip;
    uint64_t time_ping_send;
    uint64_t network_delay;

    int result;  // SUCCESS, FAILURE, WEIRD
};

// udp result info of ping
struct udp_result_info_t {
    uint32_t n_success = 0;
    uint32_t n_failure = 0;
    uint32_t n_weird = 0;

    uint64_t ts_start = 0;
    uint64_t ts_end = 0;

    std::vector<uint64_t> network_delays;

    // default constructor
    udp_result_info_t() = default;
};


/********
 * RDMA *
 ********/
// rdma ping message with client's information
union rdma_pingmsg_t {
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

// rdma server's response message (pong)
union rdma_pongmsg_t {
    char raw[20];
    struct {
        uint32_t opcode;        // PONG or ACK
        uint64_t pingid;        // ping ID
        uint64_t server_delay;  // server's process delay (for ACK)
    } x;
};

// rdma ping result msg
struct alignas(64) rdma_result_msg_t {
    uint64_t pingid;
    uint32_t dstip;

    uint64_t time_ping_send;  // timestamp of ping send
    uint64_t client_delay;
    uint64_t network_delay;
    uint64_t server_delay;

    int result;  // SUCCESS, FAILURE, WEIRD
};

// rdma result info of ping
struct rdma_result_info_t {
    uint32_t n_success = 0;
    uint32_t n_failure = 0;
    uint32_t n_weird = 0;

    uint64_t ts_start = 0;
    uint64_t ts_end = 0;

    std::vector<uint64_t> server_delays;
    std::vector<uint64_t> network_delays;
    std::vector<uint64_t> client_delays;

    // default constructor
    rdma_result_info_t() = default;
};

struct Buffer {
    char *addr;         // Pointer to the buffer
    size_t length;      // Size of the buffer
    struct ibv_mr *mr;  // Memory region after registration
};

// context for RDMA ping
struct rdma_context {
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
