#pragma once

#include <dirent.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <infiniband/verbs.h>
#include <netdb.h>
#include <signal.h>  // For kill(), signal()
#include <sys/mman.h>
#include <sys/wait.h>  // For waitpid()
#include <time.h>
#include <unistd.h>  // For fork(), sleep()
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstring>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "logger.hpp"  // logging

// constants
const static int MESSAGE_SIZE = 64;  // Message size of 64 bytes
const static int BATCH_SIZE = 1000;  // Process messages in batches of 10
const static int BUFFER_SIZE = BATCH_SIZE + 1;  // Queue size
const static int BATCH_TIMEOUT_MS = 100;        // Timeout in milliseconds
const static int GRH_SIZE = 40;                 // Specification

// RDMA parameters
const static int RX_DEPTH = 50;      // enough
const static int GID_INDEX = 0;      // by default 0
const static int SERVICE_LEVEL = 0;  // by default 0
const static int USE_EVENT = 1;  // 1: event-based polling, 2: active polling

// variables
static int use_rnic_ts = 0;  // automatically assigned
static int page_size = 0;    // automatically assigned

struct ts_params {
    uint64_t comp_recv_max_time_delta;
    uint64_t comp_recv_min_time_delta;
    uint64_t comp_recv_total_time_delta;
    uint64_t comp_recv_prev_time;
    int last_comp_with_ts;
    unsigned int comp_with_time_iters;
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
    int pending;
    int active_port;
    int is_server;
    struct ibv_port_attr portinfo;
    uint64_t completion_timestamp_mask;
    struct ts_params ts;
};

struct pingweave_dest {
    int lid;
    int qpn;
    union ibv_gid gid;
};

enum ibv_mtu pingweave_mtu_to_enum(int mtu);

void wire_gid_to_gid(const char *wgid, union ibv_gid *gid);
void gid_to_wire_gid(const union ibv_gid *gid, char wgid[]);

// Helper function to find RDMA device by matching network interface
ibv_context *get_context_by_ifname(const char *ifname);

ibv_context *get_context_by_ip(const char *ip);

// RDMA 장치에서 사용 가능한 활성화된 포트 찾기
int find_active_port(struct pingweave_context *ctx);
struct ibv_cq *pingweave_cq(struct pingweave_context *ctx);

void put_local_info(struct pingweave_dest *my_dest, int is_server,
                    std::string ip);
void get_local_info(struct pingweave_dest *rem_dest, int is_server);

int init_ctx(struct pingweave_context *ctx);

int connect_ctx(struct pingweave_context *ctx);

int post_recv(struct pingweave_context *ctx, int n);

int post_send(struct pingweave_context *ctx, struct pingweave_dest rem_dest,
              std::string msg);
