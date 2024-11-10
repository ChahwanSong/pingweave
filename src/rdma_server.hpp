#pragma once

#include "ping_info_map.hpp"
#include "ping_msg_map.hpp"
#include "rdma_common.hpp"

typedef moodycamel::ReaderWriterQueue<union ping_msg_t> ServerInternalQueue;

// Utility function: Wait for CQ event and handle it
bool wait_for_cq_event(const std::string& logname,
                       struct pingweave_context* ctx) {
    struct ibv_cq* ev_cq;
    void* ev_ctx;

    if (ibv_get_cq_event(ctx->channel, &ev_cq, &ev_ctx)) {
        spdlog::get(logname)->error("Failed to get cq_event");
        return false;
    }

    // Acknowledge the CQ event
    ibv_ack_cq_events(pingweave_cq(ctx), 1);

    // Verify that the event is from the correct CQ
    if (ev_cq != pingweave_cq(ctx)) {
        spdlog::get(logname)->error("CQ event for unknown CQ");
        return false;
    }

    // Re-register for CQ event notifications
    if (ibv_req_notify_cq(pingweave_cq(ctx), 0)) {
        spdlog::get(logname)->error("Couldn't register CQE notification");
        return false;
    }

    return true;
}

// Utility function: Poll CQE
bool poll_cq(const std::string& logname, struct pingweave_context* ctx,
             struct ibv_wc& wc, uint64_t& cqe_time) {
    int ret;
    if (ctx->rnic_hw_ts) {  // Using RNIC timestamping
        struct ibv_poll_cq_attr attr = {};
        ret = ibv_start_poll(ctx->cq_s.cq_ex, &attr);
        if (ret) {
            spdlog::get(logname)->error("ibv_start_poll failed: {}", ret);
            return false;
        }

        wc.status = ctx->cq_s.cq_ex->status;
        wc.wr_id = ctx->cq_s.cq_ex->wr_id;
        wc.opcode = ibv_wc_read_opcode(ctx->cq_s.cq_ex);
        cqe_time = ibv_wc_read_completion_ts(ctx->cq_s.cq_ex);

        ibv_end_poll(ctx->cq_s.cq_ex);
    } else {  // Using application-level timestamping
        if (ibv_poll_cq(pingweave_cq(ctx), 1, &wc) != 1) {
            spdlog::get(logname)->error("CQE poll receives nothing");
            return false;
        }
        cqe_time = get_current_timestamp_steady();
    }
    return true;
}

// Utility function: Post RECV WR
bool post_recv_wr(const std::string& logname, struct pingweave_context* ctx) {
    if (post_recv(ctx, 1, PINGWEAVE_WRID_RECV) == 0) {
        spdlog::get(logname)->warn("RECV post failed.");
        return false;
    }
    return true;
}

// Utility function: Handle PING message
bool handle_ping_message(const std::string& logname,
                         struct pingweave_context* ctx, struct ibv_wc& wc,
                         uint64_t cqe_time, ServerInternalQueue* server_queue) {
    spdlog::get(logname)->debug("[CQE] RECV (wr_id: {})", wc.wr_id);

    // Parse GRH header (for debugging)
    struct ibv_grh* grh = reinterpret_cast<struct ibv_grh*>(ctx->buf);
    spdlog::get(logname)->debug("  -> from: {}", parsed_gid(&grh->sgid));

    // Parse PING message
    union ping_msg_t ping_msg;
    std::memcpy(&ping_msg, ctx->buf + GRH_SIZE, sizeof(ping_msg_t));
    ping_msg.x.time = cqe_time;

    // For debugging
    spdlog::get(logname)->debug("  -> id: {}, gid: {}, qpn: {}, time: {}",
                                ping_msg.x.pingid, parsed_gid(&ping_msg.x.gid),
                                ping_msg.x.qpn, ping_msg.x.time);

    if (!server_queue->try_enqueue(ping_msg)) {
        spdlog::get(logname)->error("Failed to enqueue ping message");
        return false;
    }
    return true;
}

// Utility function: Process PONG CQE
bool process_pong_cqe(const std::string& logname,
                      struct pingweave_context& ctx_tx, struct ibv_wc& wc,
                      uint64_t cqe_time, PingMsgMap& pong_table) {
    spdlog::get(logname)->debug("-> PONG's pingID: {}", wc.wr_id);
    union ping_msg_t ping_msg = {};
    if (pong_table.get(wc.wr_id, ping_msg)) {
        // Create ACK message
        union pong_msg_t pong_msg = {};
        pong_msg.x.opcode = PINGWEAVE_OPCODE_ACK;
        pong_msg.x.pingid = wc.wr_id;
        pong_msg.x.server_delay = calc_time_delta_with_bitwrap(
            ping_msg.x.time, cqe_time, ctx_tx.completion_timestamp_mask);
        spdlog::get(logname)->debug(
            "SEND post with ACK message pingid: {} to qpn: {}, gid: {}, delay: "
            "{}",
            pong_msg.x.pingid, ping_msg.x.qpn, parsed_gid(&ping_msg.x.gid),
            pong_msg.x.server_delay);

        union rdma_addr dst_addr;
        dst_addr.x.gid = ping_msg.x.gid;
        dst_addr.x.qpn = ping_msg.x.qpn;

        /**
         * Small jittering to prevent buffer override at client-side.
         * This can happen as we use RDMA UC communication.
         **/
        std::this_thread::sleep_for(std::chrono::microseconds(1));

        if (post_send(&ctx_tx, dst_addr, pong_msg.raw, sizeof(pong_msg_t),
                      PINGWEAVE_WRID_SEND)) {
            if (check_log(ctx_tx.log_msg)) {
                spdlog::get(logname)->error(ctx_tx.log_msg);
            }
            return false;
        }
    } else {
        spdlog::get(logname)->warn("pingid {} entry does not exist (expired?)",
                                   wc.wr_id);
    }
    // Remove entry from table
    if (!pong_table.remove(wc.wr_id)) {
        spdlog::get(logname)->warn(
            "Nothing to remove the id {} from pong_table", wc.wr_id);
    }
    return true;
}

// Server RX thread
void server_rx_thread(const std::string& ipv4, const std::string& logname,
                      ServerInternalQueue* server_queue,
                      struct pingweave_context* ctx_rx) {
    spdlog::get(logname)->info("Running RX thread (pid: {})...", getpid());

    try {
        // Initial RECV WR posting
        if (!post_recv_wr(logname, ctx_rx)) {
            throw std::runtime_error("Initial RECV post failed");
        }

        // Register for CQ event notifications
        if (ibv_req_notify_cq(pingweave_cq(ctx_rx), 0)) {
            spdlog::get(logname)->error("Couldn't register CQE notification");
            throw std::runtime_error("Couldn't register CQE notification");
        }

        // Polling loop
        while (true) {
            spdlog::get(logname)->debug("Waiting to poll RX RECV CQE...");

            // Wait for CQ event
            if (!wait_for_cq_event(logname, ctx_rx)) {
                throw std::runtime_error("Failed during CQ event waiting");
            }

            // Poll CQE
            struct ibv_wc wc = {};
            uint64_t cqe_time = 0;
            if (!poll_cq(logname, ctx_rx, wc, cqe_time)) {
                throw std::runtime_error("Failed during CQ polling");
            }

            // Post next RECV WR
            if (!post_recv_wr(logname, ctx_rx)) {
                throw std::runtime_error("Failed to post next RECV WR");
            }

            // Check WC status and handle
            if (wc.status == IBV_WC_SUCCESS) {
                if (wc.opcode == IBV_WC_RECV) {
                    if (!handle_ping_message(logname, ctx_rx, wc, cqe_time,
                                             server_queue)) {
                        throw std::runtime_error(
                            "Failed to handle PING message");
                    }
                } else {
                    spdlog::get(logname)->error("Unexpected opcode: {}",
                                                static_cast<int>(wc.opcode));
                    throw std::runtime_error("Unexpected opcode in RX thread");
                }
            } else {
                spdlog::get(logname)->warn(
                    "RX WR failure - status: {}, opcode: {}",
                    ibv_wc_status_str(wc.status), static_cast<int>(wc.opcode));
                throw std::runtime_error("RX WR failure");
            }
        }
    } catch (const std::exception& e) {
        spdlog::get(logname)->error("RX thread exits unexpectedly: {}",
                                    e.what());
        throw;  // Propagate exception
    }
}

// RDMA server main function
void rdma_server(const std::string& ipv4) {
    // Initialize logger
    spdlog::drop_all();
    const std::string logname = "rdma_server_" + ipv4;
    auto logger = initialize_custom_logger(logname, LOG_LEVEL_SERVER,
                                           LOG_FILE_SIZE, LOG_FILE_EXTRA_NUM);

    // Create internal queue
    ServerInternalQueue server_queue(QUEUE_SIZE);

    // Initialize RDMA context
    struct pingweave_context ctx_tx, ctx_rx;
    if (make_ctx(&ctx_tx, ipv4, logname, false)) {
        logger->error("Failed to make TX device info: {}", ipv4);
        throw std::runtime_error("make_ctx failed.");
    }

    if (make_ctx(&ctx_rx, ipv4, logname, true)) {
        logger->error("Failed to make RX device info: {}", ipv4);
        throw std::runtime_error("make_ctx failed.");
    }
    if (save_device_info(&ctx_rx)) {
        logger->error(ctx_rx.log_msg);
        throw std::runtime_error("save_device_info failed.");
    }

    // Start RX thread
    std::thread rx_thread(server_rx_thread, ipv4, logname, &server_queue,
                          &ctx_rx);

    // Main thread loop - handle messages
    logger->info("Running main (TX) thread (pid: {})...", getpid());

    // Create the table (entry timeout = 1 second)
    PingMsgMap pong_table(1);

    // Variables
    union ping_msg_t ping_msg;
    union pong_msg_t pong_msg;
    uint64_t cqe_time;
    struct ibv_wc wc = {};
    int ret;

    while (true) {
        // Receive and process PING message from internal queue
        if (server_queue.try_dequeue(ping_msg)) {
            logger->debug(
                "Internal queue received a ping_msg - pingid: {}, qpn: {}, "
                "gid: {}, ping arrival time: {}",
                ping_msg.x.pingid, ping_msg.x.qpn, parsed_gid(&ping_msg.x.gid),
                ping_msg.x.time);

            // (1) Store in table
            if (!pong_table.insert(ping_msg.x.pingid, ping_msg)) {
                spdlog::get(logname)->warn(
                    "Failed to insert pingid {} into pong_table.",
                    ping_msg.x.pingid);
            }

            // (2) Create and send PONG message
            pong_msg = {};
            pong_msg.x.opcode = PINGWEAVE_OPCODE_PONG;
            pong_msg.x.pingid = ping_msg.x.pingid;
            pong_msg.x.server_delay = 0;

            union rdma_addr dst_addr;
            dst_addr.x.gid = ping_msg.x.gid;
            dst_addr.x.qpn = ping_msg.x.qpn;

            // send PONG message
            spdlog::get(logname)->debug(
                "SEND post with PONG message of pingid: {} to qpn: {}, gid: {}",
                pong_msg.x.pingid, dst_addr.x.qpn, parsed_gid(&dst_addr.x.gid));
            if (post_send(&ctx_tx, dst_addr, pong_msg.raw, sizeof(pong_msg_t),
                          pong_msg.x.pingid)) {
                if (check_log(ctx_tx.log_msg)) {
                    spdlog::get(logname)->error(ctx_tx.log_msg);
                }
                throw std::runtime_error("SEND PONG post failed");
            }
        }

        // Capture and process CQE
        if (ctx_tx.rnic_hw_ts) {
            // Poll CQE when using RNIC timestamping
            struct ibv_poll_cq_attr attr = {};
            ret = ibv_start_poll(ctx_tx.cq_s.cq_ex, &attr);
            bool has_events = false;

            while (!ret) {
                has_events = true;

                // Extract WC information
                wc.status = ctx_tx.cq_s.cq_ex->status;
                wc.wr_id = ctx_tx.cq_s.cq_ex->wr_id;
                wc.opcode = ibv_wc_read_opcode(ctx_tx.cq_s.cq_ex);
                cqe_time = ibv_wc_read_completion_ts(ctx_tx.cq_s.cq_ex);

                // Check WC status and handle
                if (wc.status == IBV_WC_SUCCESS) {
                    if (wc.opcode == IBV_WC_SEND) {
                        if (wc.wr_id == PINGWEAVE_WRID_SEND) {
                            spdlog::get(logname)->debug(
                                "CQE of ACK, so ignore this");
                        } else {
                            if (process_pong_cqe(logname, ctx_tx, wc, cqe_time,
                                                 pong_table)) {
                                // Successfully processed
                            } else {
                                throw std::runtime_error(
                                    "Failed to process PONG CQE");
                            }
                        }
                    } else {
                        spdlog::get(logname)->error(
                            "Unexpected opcode: {}",
                            static_cast<int>(wc.opcode));
                        throw std::runtime_error("Unexpected opcode");
                    }
                } else {
                    spdlog::get(logname)->warn(
                        "TX WR failure - status: {}, opcode: {}",
                        ibv_wc_status_str(wc.status),
                        static_cast<int>(wc.opcode));
                    throw std::runtime_error("TX WR failure");
                }

                // Poll next event
                ret = ibv_next_poll(ctx_tx.cq_s.cq_ex);

                if (!ret) {
                    std::cout << "next poll success" << std::endl;
                }
            }
            if (has_events) {
                ibv_end_poll(ctx_tx.cq_s.cq_ex);
            } else {  // nothing to poll
                // to minize CPU overhead for polling
                std::this_thread::sleep_for(std::chrono::microseconds(20));
                continue;
            }
        } else {
            // Poll CQE when using application-level timestamping
            ret = ibv_poll_cq(pingweave_cq(&ctx_tx), 1, &wc);

            if (!ret) {  // nothing to poll
                // to minize CPU overhead for polling
                std::this_thread::sleep_for(std::chrono::microseconds(20));
                continue;
            }

            while (ret) {
                cqe_time = get_current_timestamp_steady();
                if (wc.status == IBV_WC_SUCCESS) {
                    if (wc.opcode == IBV_WC_SEND) {
                        if (wc.wr_id == PINGWEAVE_WRID_SEND) {
                            spdlog::get(logname)->debug(
                                "CQE of ACK, so ignore this");
                        } else {
                            if (process_pong_cqe(logname, ctx_tx, wc, cqe_time,
                                                 pong_table)) {
                                // Successfully processed
                            } else {
                                throw std::runtime_error(
                                    "Failed to process PONG CQE");
                            }
                        }
                    } else {
                        spdlog::get(logname)->error(
                            "Unexpected opcode: {}",
                            static_cast<int>(wc.opcode));
                        throw std::runtime_error("Unexpected opcode");
                    }
                } else {
                    spdlog::get(logname)->warn(
                        "TX WR failure - status: {}, opcode: {}",
                        ibv_wc_status_str(wc.status),
                        static_cast<int>(wc.opcode));
                    throw std::runtime_error("TX WR failure");
                }

                // Poll next event
                ret = ibv_poll_cq(pingweave_cq(&ctx_tx), 1, &wc);
            }
        }
    }

    // Handle RX thread termination
    if (rx_thread.joinable()) {
        rx_thread.join();
    }
}
