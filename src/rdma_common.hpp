#pragma once

#include <infiniband/verbs.h>

#include "common.hpp"

// fully-blocking SPSC queue
typedef moodycamel::BlockingReaderWriterQueue<struct rdma_result_msg_t>
    RdmaClientQueue;
typedef moodycamel::ReaderWriterQueue<union rdma_pingmsg_t> RdmaServerQueue;

const static int GRH_SIZE = sizeof(ibv_grh);  // GRH header 40 B (see IB Spec)

// Helper function to find RDMA device by matching network interface
int get_context_by_ifname(const char *ifname, struct rdma_context *ctx);
int get_context_by_ip(struct rdma_context *ctx);
int find_active_port(struct rdma_context *ctx,
                     std::shared_ptr<spdlog::logger> logger);
int get_gid_table_size(struct rdma_context *ctx,
                       std::shared_ptr<spdlog::logger> logger);

// Allocate RDMA resources
struct ibv_cq *pingweave_cq(struct rdma_context *ctx);
int init_ctx(struct rdma_context *ctx);
int prepare_ctx(struct rdma_context *ctx);
int make_ctx(struct rdma_context *ctx, const std::string &ipv4,
             const int &is_rx, std::shared_ptr<spdlog::logger> logger);

// Initialize TX/RX context for RDMA UD
int initialize_contexts(rdma_context &ctx_tx, rdma_context &ctx_rx,
                        const std::string &ipv4,
                        std::shared_ptr<spdlog::logger> logger);

// RDMA Ops
int post_recv(struct rdma_context *ctx, const uint64_t &wr_id, const int &n);
int post_send(struct rdma_context *ctx, union rdma_addr rem_dest,
              const char *msg, const size_t &msg_len, const int &buf_idx,
              const uint64_t &wr_id, std::shared_ptr<spdlog::logger> logger);
int wait_for_cq_event(struct rdma_context *ctx,
                      std::shared_ptr<spdlog::logger> logger);
int save_device_info(struct rdma_context *ctx,
                     std::shared_ptr<spdlog::logger> logger);
int load_device_info(union rdma_addr *dst_addr, const std::string &filepath,
                     std::shared_ptr<spdlog::logger> logger);
void wire_gid_to_gid(const char *wgid, union ibv_gid *gid);
void gid_to_wire_gid(const union ibv_gid *gid, char wgid[]);
std::string parsed_gid(union ibv_gid *gid);

// statistics
std::string convert_rdma_result_to_str(const std::string &srcip,
                                       const std::string &dstip,
                                       const rdma_result_info_t &result_info,
                                       const result_stat_t &client_stat,
                                       const result_stat_t &network_stat,
                                       const result_stat_t &server_stat);
