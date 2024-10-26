#pragma once

#include <arpa/inet.h>
#include <dirent.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <infiniband/verbs.h>
#include <netdb.h>
#include <signal.h>  // For kill(), signal()
#include <sys/mman.h>
#include <sys/stat.h>  // 디렉토리 확인 및 생성
#include <sys/types.h>
#include <sys/wait.h>  // For waitpid()
#include <time.h>
#include <unistd.h>  // For fork(), sleep()
#include <unistd.h>

#include <atomic>  // std::atomic
#include <cerrno>  // errno
#include <chrono>
#include <condition_variable>  // std::scoped_lock
#include <cstring>             // strerror
#include <cstring>
#include <filesystem>
#include <fstream>
#include <future>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "../libs/readerwriterqueue/readerwriterqueue.h"
#include "logger.hpp"

#define MSG_INPUT(out, msg, args...) \
    snprintf(out, 100, "%s : %d : " msg, __FILE__, __LINE__, ##args);

// internal queue typedef
typedef moodycamel::ReaderWriterQueue<union server_interval_msg_t> ServerQueue;

// constants
const static int MESSAGE_SIZE = 64;           // Message size of 64 B
const static int GRH_SIZE = sizeof(ibv_grh);  // GRH header 40 B (see IB Spec)

// RDMA parameters
const static int TX_DEPTH = 1;       // only 1 SEND to have data consistency
const static int RX_DEPTH = 10;      // enough?
const static int GID_INDEX = 0;      // by default 0
const static int SERVICE_LEVEL = 0;  // by default 0
const static int USE_EVENT = 1;  // 1: event-based polling, 2: active polling

// Params for internal message queue btw threads (RX <-> TX)
const static int QUEUE_SIZE = 100;

// Params for IPC (inter-processor communication)
const static int BATCH_SIZE = 1000;             // Process messages in batches
const static int BUFFER_SIZE = BATCH_SIZE + 1;  // Message queue's buffer size
const static int BATCH_TIMEOUT_MS = 100;        // Timeout in milliseconds
const std::string PREFIX_SHMEM_NAME = "/pingweave_";  // Name of shared memory

enum {
    PINGWEAVE_WRID_RECV = 1,
    PINGWEAVE_WRID_SEND = 2,
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
    char raw[20];
    struct {
        uint32_t qpn;       // 4B
        union ibv_gid gid;  // 16B
    } x;
};

union alignas(8) ping_msg_t {
    char raw[8];
    struct {
        uint32_t pingid;  // 4B
        uint32_t qpn;     // 4B
    } x;
};

// Server's internal message format
union alignas(32) server_interval_msg_t {
    char raw[32];
    struct {
        uint32_t pingid;    // 4B
        uint32_t qpn;       // 4B
        union ibv_gid gid;  // 16B
        uint64_t time;      // 8B
    } x;
};

// enum ibv_mtu pingweave_mtu_to_enum(int mtu);

void wire_gid_to_gid(const char *wgid, union ibv_gid *gid);
void gid_to_wire_gid(const union ibv_gid *gid, char wgid[]);

// Helper function to find RDMA device by matching network interface
int get_context_by_ifname(const char *ifname, struct pingweave_context *ctx);
int get_context_by_ip(struct pingweave_context *ctx);

// Find the active port from RNIC hardware
int find_active_port(struct pingweave_context *ctx);

struct ibv_cq *pingweave_cq(struct pingweave_context *ctx);

// void put_local_info(struct pingweave_addr *my_dest, int is_server,
//                     std::string ip);
// void get_local_info(struct pingweave_addr *rem_dest, int is_server);

int init_ctx(struct pingweave_context *ctx);

int prepare_ctx(struct pingweave_context *ctx);

int make_ctx(struct pingweave_context *ctx, const std::string &ipv4,
             const std::string &logname, const int &is_server,
             const int &is_rx);

int post_recv(struct pingweave_context *ctx, int n, const uint64_t &wr_id);
int post_send(struct pingweave_context *ctx, union rdma_addr rem_dest,
              const char *msg, const size_t &msg_len, const uint64_t &wr_id);

int save_device_info(struct pingweave_context *ctx);
int load_device_info(union rdma_addr *dst_addr, const std::string &filepath);
