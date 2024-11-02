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
#include <cstring>  // strerror
#include <cstring>
#include <filesystem>
#include <fstream>
#include <future>
#include <iostream>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <tuple>
#include <unordered_map>
#include <vector>

#include "../libs/readerwriterqueue/readerwriterqueue.h"
#include "logger.hpp"
#include "ping_info_map.hpp"
#include "ping_msg_map.hpp"

// internal queue typedef
typedef moodycamel::ReaderWriterQueue<union ping_msg_t> ServerInternalQueue;
typedef moodycamel::ReaderWriterQueue<struct ping_info_t> ClientInternalQueue;

// constants
const static int MESSAGE_SIZE = 64;           // Message size of 64 B
const static int GRH_SIZE = sizeof(ibv_grh);  // GRH header 40 B (see IB Spec)
const static uint64_t PING_ID_INIT = 1000000000;  // start id

// RDMA parameters
const static int TX_DEPTH = 1;       // only 1 SEND to have data consistency
const static int RX_DEPTH = 10;      // enough?
const static int GID_INDEX = 0;      // by default 0
const static int SERVICE_LEVEL = 0;  // by default 0
const static int USE_EVENT = 1;  // 1: event-based polling, 2: active polling

// Params for internal message queue btw threads (RX <-> TX)
const static int QUEUE_SIZE = 1000;

// Params for IPC (inter-processor communication)
const static int BATCH_SIZE = 1000;             // Process messages in batches
const static int BUFFER_SIZE = BATCH_SIZE + 1;  // Message queue's buffer size
const static int BATCH_TIMEOUT_MS = 100;        // Timeout in milliseconds
const std::string PREFIX_SHMEM_NAME = "/pingweave_";  // Name of shared memory

enum {
    PINGWEAVE_WRID_RECV = 1,
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
    char raw[20];
    struct {
        uint32_t qpn;       // 4B
        union ibv_gid gid;  // 16B
    } x;
};

// enum ibv_mtu pingweave_mtu_to_enum(int mtu);

void wire_gid_to_gid(const char *wgid, union ibv_gid *gid);
void gid_to_wire_gid(const union ibv_gid *gid, char wgid[]);
std::string parsed_gid(union ibv_gid *gid);

// Helper function to find RDMA device by matching network interface
int get_context_by_ifname(const char *ifname, struct pingweave_context *ctx);
int get_context_by_ip(struct pingweave_context *ctx);

// clock
uint64_t calc_time_delta_with_bitwrap(const uint64_t &t1, const uint64_t &t2,
                                      const uint64_t &mask);
uint32_t get_current_time();

// Find the active port from RNIC hardware
int find_active_port(struct pingweave_context *ctx);

struct ibv_cq *pingweave_cq(struct pingweave_context *ctx);

// void put_local_info(struct pingweave_addr *my_dest, int is_server,
//                     std::string ip);
// void get_local_info(struct pingweave_addr *rem_dest, int is_server);

int init_ctx(struct pingweave_context *ctx);
int prepare_ctx(struct pingweave_context *ctx);
int make_ctx(struct pingweave_context *ctx, const std::string &ipv4,
             const std::string &logname, const int &is_rx);

int post_recv(struct pingweave_context *ctx, int n, const uint64_t &wr_id);
int post_send(struct pingweave_context *ctx, union rdma_addr rem_dest,
              const char *msg, const size_t &msg_len, const uint64_t &wr_id);

int save_device_info(struct pingweave_context *ctx);
// for testing
int load_device_info(union rdma_addr *dst_addr, const std::string &filepath);

std::set<std::string> get_all_local_ips();

void get_my_addr(const std::string &filename, std::set<std::string> &myaddr);
// void parse_rdma_pinglist(const std::string &filename,
//                          std::set<std::string> &myaddr,
//                          std::vector<std::string> &pinglist);
