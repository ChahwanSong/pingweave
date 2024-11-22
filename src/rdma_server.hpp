#pragma once

#include "ping_info_map.hpp"
#include "ping_msg_map.hpp"
#include "rdma_common.hpp"

void server_process_rx_cqe(pingweave_context* ctx_rx,
                           ServerInternalQueue* server_queue,
                           std::shared_ptr<spdlog::logger> logger) {
    uint64_t cqe_time = 0;
    struct ibv_wc wc = {};
    int ret = 0;
    union ping_msg_t ping_msg = {};
    int num_cqes = 0;

    try {
        if (ctx_rx->rnic_hw_ts) {
            // Use extended CQ polling for hardware timestamping
            struct ibv_poll_cq_attr attr = {};
            bool has_events = false;
            ret = ibv_start_poll(ctx_rx->cq_s.cq_ex, &attr);

            while (!ret) {
                ++num_cqes;
                logger->debug("CQ Event loop {}", num_cqes);
                has_events = true;

                // Extract WC information
                wc.status = ctx_rx->cq_s.cq_ex->status;

                // if failure
                if (wc.status != IBV_WC_SUCCESS) {
                    logger->error("CQE RX error: {}",
                                  ibv_wc_status_str(wc.status));
                    /** TODO: is it correct? */
                    ret = ibv_next_poll(ctx_rx->cq_s.cq_ex);
                    continue;
                }

                wc.opcode = ibv_wc_read_opcode(ctx_rx->cq_s.cq_ex);
                wc.wr_id = ctx_rx->cq_s.cq_ex->wr_id;

                if (wc.opcode == IBV_WC_RECV) {
                    logger->debug("[CQE] RECV (wr_id: {})", wc.wr_id);

                    // Parse the received message
                    auto buf = ctx_rx->buf[wc.wr_id];
                    std::memcpy(&ping_msg, buf.addr + GRH_SIZE,
                                sizeof(ping_msg_t));
                    ping_msg.x.time = ibv_wc_read_completion_ts(
                        ctx_rx->cq_s.cq_ex);  // Get current time

                    // Parse GRH header (for debugging)
                    struct ibv_grh* grh = reinterpret_cast<struct ibv_grh*>(
                        ctx_rx->buf[wc.wr_id].addr);
                    logger->debug("  -> from: {}", parsed_gid(&grh->sgid));
                    logger->debug(
                        "  -> id: {}, gid: {}, lid: {}, qpn: {}, time: "
                        "{}",
                        ping_msg.x.pingid, parsed_gid(&ping_msg.x.gid),
                        ping_msg.x.lid, ping_msg.x.qpn, ping_msg.x.time);

                    // Post the next RECV WR
                    if (post_recv(ctx_rx, wc.wr_id, 1) == 0) {
                        logger->warn("Failed to repost the next RECV WR.");
                    }

                    if (!server_queue->try_enqueue(ping_msg)) {
                        logger->error("Failed to enqueue ping message");
                        throw std::runtime_error(
                            "Failed to handle PING message");
                    }
                } else {
                    logger->error("Unexpected opcode: {}",
                                  static_cast<int>(wc.opcode));
                    throw std::runtime_error("Unexpected opcode in RX thread");
                }
                ret = ibv_next_poll(ctx_rx->cq_s.cq_ex);
            }

            if (has_events) {
                // End the polling session
                ibv_end_poll(ctx_rx->cq_s.cq_ex);
            } else {  // nothing to poll
                logger->error("RX: CQE poll receives nothing");
                throw std::runtime_error("Failed during CQ polling");
            }
        } else {
            struct ibv_wc wc_array[BATCH_CQE];
            num_cqes = ibv_poll_cq(ctx_rx->cq, BATCH_CQE, wc_array);

            if (num_cqes < 0) {
                logger->error("Failed to poll CQ");
                throw std::runtime_error("Failed to poll CQ");
            } else if (num_cqes == 0) {  // no completion
                logger->error("CQE poll receives nothing");
                throw std::runtime_error("Failed during CQ polling");
            }

            for (int i = 0; i < num_cqes; ++i) {
                struct ibv_wc& wc = wc_array[i];

                // if failure
                if (wc.status != IBV_WC_SUCCESS) {
                    logger->error("CQE RX error: {}",
                                  ibv_wc_status_str(wc.status));
                    /** TODO: is it correct? */
                    continue;
                }

                if (wc.opcode == IBV_WC_RECV) {
                    logger->debug("[CQE] RECV (wr_id: {})", wc.wr_id);

                    // Parse the received message
                    auto buf = ctx_rx->buf[wc.wr_id];
                    std::memcpy(&ping_msg, buf.addr + GRH_SIZE,
                                sizeof(ping_msg_t));
                    ping_msg.x.time =
                        get_current_timestamp_steady();  // Get current time

                    // Parse GRH header (for debugging)
                    struct ibv_grh* grh = reinterpret_cast<struct ibv_grh*>(
                        ctx_rx->buf[wc.wr_id].addr);
                    logger->debug("  -> from: {}", parsed_gid(&grh->sgid));
                    logger->debug(
                        "  -> id: {}, gid: {}, lid: {}, qpn: {}, time: "
                        "{}",
                        ping_msg.x.pingid, parsed_gid(&ping_msg.x.gid),
                        ping_msg.x.lid, ping_msg.x.qpn, ping_msg.x.time);

                    // Post the next RECV WR
                    if (post_recv(ctx_rx, wc.wr_id, 1) == 0) {
                        logger->warn("Failed to repost the next RECV WR.");
                    }

                    // Handle the received message (PONG or ACK)
                    if (!server_queue->try_enqueue(ping_msg)) {
                        logger->error("Failed to enqueue ping message");
                        throw std::runtime_error(
                            "Failed to handle PING message");
                    }
                } else {
                    logger->error("Unexpected opcode: {}",
                                  static_cast<int>(wc.opcode));
                    throw std::runtime_error("Unexpected opcode in RX thread");
                }
            }
        }

        // Acknowledge the CQ event
        ibv_ack_cq_events(pingweave_cq(ctx_rx), num_cqes);

        // Re-register for CQ event notifications
        if (ibv_req_notify_cq(pingweave_cq(ctx_rx), 0)) {
            logger->error("Couldn't register CQE notification");
            throw std::runtime_error(
                "Failed to post cqe request notification.");
        }

    } catch (const std::exception& e) {
        logger->error("RX CQE handler exits unexpectedly: {}", e.what());
        throw;  // Propagate exception
    }
}

// Utility function: Process PONG CQE
bool server_process_pong_cqe(struct pingweave_context* ctx_tx,
                             const struct ibv_wc& wc, const uint64_t& cqe_time,
                             PingMsgMap* ping_table,
                             std::shared_ptr<spdlog::logger> logger) {
    logger->debug("[CQE] PONG's pingID: {}", wc.wr_id);
    union ping_msg_t ping_msg = {0};

    if (ping_table->get(wc.wr_id, ping_msg)) {
        // Create ACK message
        union pong_msg_t pong_msg = {};
        pong_msg.x.opcode = PINGWEAVE_OPCODE_ACK;
        pong_msg.x.pingid = wc.wr_id;
        pong_msg.x.server_delay = calc_time_delta_with_bitwrap(
            ping_msg.x.time, cqe_time, ctx_tx->completion_timestamp_mask);
        logger->debug(
            "-> SEND post with ACK message pingid: {} to qpn: {}, gid: {}, "
            "lid: "
            "{}, delay: {}",
            pong_msg.x.pingid, ping_msg.x.qpn, parsed_gid(&ping_msg.x.gid),
            ping_msg.x.lid, pong_msg.x.server_delay);

        union rdma_addr dst_addr;
        dst_addr.x.gid = ping_msg.x.gid;
        dst_addr.x.lid = ping_msg.x.lid;
        dst_addr.x.qpn = ping_msg.x.qpn;

        /**
         * TODO: Small jittering to prevent buffer override at client-side.
         * This can happen as we use RDMA UC communication.
         **/
        // std::this_thread::sleep_for(std::chrono::microseconds(10));

        // send PONG ACK
        if (post_send(ctx_tx, dst_addr, pong_msg.raw, sizeof(pong_msg_t),
                      wc.wr_id % ctx_tx->buf.size(), PINGWEAVE_WRID_PONG_ACK,
                      logger)) {
            return false;  // failed
        }
    } else {
        logger->warn("pingid {} entry does not exist at ping_table (expired?)",
                     wc.wr_id);
    }
    // Remove entry from table
    if (!ping_table->remove(wc.wr_id)) {
        logger->warn("Nothing to remove the id {} from ping_table", wc.wr_id);
    }

    // success
    return true;
}

void process_tx_cqe(pingweave_context* ctx_tx, PingMsgMap* ping_table,
                    std::shared_ptr<spdlog::logger> logger) {
    struct ibv_wc wc = {};
    uint64_t cqe_time = 0;
    int ret = 0;

    /**
     * IMPORTANT: Use non-blocking polling.
     * Otherwise, scheduling the next PONG to send will be blocked.
     */
    if (ctx_tx->rnic_hw_ts) {
        // Use extended CQ polling for hardware timestamping
        struct ibv_poll_cq_attr attr = {};
        ret = ibv_start_poll(ctx_tx->cq_s.cq_ex, &attr);
        bool has_events = false;

        while (!ret) {
            has_events = true;

            // Extract WC information
            wc.status = ctx_tx->cq_s.cq_ex->status;
            wc.wr_id = ctx_tx->cq_s.cq_ex->wr_id;
            wc.opcode = ibv_wc_read_opcode(ctx_tx->cq_s.cq_ex);
            cqe_time = ibv_wc_read_completion_ts(ctx_tx->cq_s.cq_ex);

            // if failure
            if (wc.status != IBV_WC_SUCCESS) {
                logger->error("CQE TX error: {}", ibv_wc_status_str(wc.status));
                ret = ibv_next_poll(ctx_tx->cq_s.cq_ex);
                continue;
            }

            if (wc.opcode == IBV_WC_SEND) {
                // PONG ACK's CQE -> ignore
                if (wc.wr_id == PINGWEAVE_WRID_PONG_ACK) {
                    logger->debug("[CQE] CQE of ACK. Do nothing.");
                    ret = ibv_next_poll(ctx_tx->cq_s.cq_ex);
                    continue;
                }

                // PONG's CQE
                if (!server_process_pong_cqe(ctx_tx, wc, cqe_time, ping_table,
                                             logger)) {
                    throw std::runtime_error("Failed to process PONG CQE");
                }

            } else {
                logger->error("Unexpected opcode: {}",
                              static_cast<int>(wc.opcode));
                throw std::runtime_error("Unexpected opcode");
            }

            // Poll next event
            ret = ibv_next_poll(ctx_tx->cq_s.cq_ex);
        }
        if (has_events) {
            ibv_end_poll(ctx_tx->cq_s.cq_ex);
        } else {
            // nothing to poll. add a small jittering.
            std::this_thread::sleep_for(std::chrono::microseconds(10));
            return;
        }
    } else {
        struct ibv_wc wc_array[BATCH_CQE];
        int num_cqes = ibv_poll_cq(ctx_tx->cq, BATCH_CQE, wc_array);

        if (num_cqes < 0) {
            logger->error("Failed to poll CQ");
            throw std::runtime_error("Failed to poll CQ");
        } else if (num_cqes == 0) {  // no completion
            std::this_thread::sleep_for(std::chrono::microseconds(10));
            return;
        }

        for (int i = 0; i < num_cqes; ++i) {
            struct ibv_wc& wc = wc_array[i];

            // if failure
            if (wc.status != IBV_WC_SUCCESS) {
                logger->error("CQE TX error: {}", ibv_wc_status_str(wc.status));
                ret = ibv_next_poll(ctx_tx->cq_s.cq_ex);
                continue;
            }

            if (wc.opcode == IBV_WC_SEND) {
                logger->debug("[CQE] Send completed (ping ID: {}), time: {}",
                              wc.wr_id, cqe_time);

                // get CQE time
                cqe_time = get_current_timestamp_steady();

                // PONG ACK's CQE -> ignore
                if (wc.wr_id == PINGWEAVE_WRID_PONG_ACK) {
                    logger->debug("CQE of ACK. Do nothing.");
                    ret = ibv_next_poll(ctx_tx->cq_s.cq_ex);
                    continue;
                }

                // PONG's CQE
                if (!server_process_pong_cqe(ctx_tx, wc, cqe_time, ping_table,
                                             logger)) {
                    throw std::runtime_error("Failed to process PONG CQE");
                }

            } else {
                logger->error("Unexpected opcode: {}",
                              static_cast<int>(wc.opcode));
                throw std::runtime_error("Unexpected opcode");
            }
        }
    }
}

// Server RX thread
void server_rx_thread(struct pingweave_context* ctx_rx, const std::string& ipv4,
                      ServerInternalQueue* server_queue,
                      std::shared_ptr<spdlog::logger> logger) {
    logger->info("Running RX thread (Thread ID: {})...", get_thread_id());

    // RECV WR uses wr_id as a buffer index
    for (int i = 0; i < ctx_rx->buf.size(); ++i) {
        if (post_recv(ctx_rx, i, RX_DEPTH) != RX_DEPTH) {
            logger->error("Failed to post RECV WRs.");
            throw std::runtime_error(
                "Failed to post RECV WR when initialization.");
        }
    }

    /** IMPORTANT: Use event-driven polling to reduce CPU overhead */
    // Register for CQ event notifications
    if (ibv_req_notify_cq(pingweave_cq(ctx_rx), 0)) {
        logger->error("Couldn't register CQE notification");
        throw std::runtime_error("Couldn't register CQE notification");
    }

    try {
        // Polling loop
        while (true) {
            // Wait for the next CQE
            if (!wait_for_cq_event(ctx_rx, logger)) {
                throw std::runtime_error("Failed during CQ event waiting");
            }

            // Process RX CQEs
            server_process_rx_cqe(ctx_rx, server_queue, logger);
        }
    } catch (const std::exception& e) {
        logger->error("Exception in RX thread: {}", e.what());
        throw;  // Propagate exception
    }
}

void server_tx_thread(struct pingweave_context* ctx_tx, const std::string& ipv4,
                      ServerInternalQueue* server_queue,
                      std::shared_ptr<spdlog::logger> logger) {
    // TX thread loop - handle messages
    logger->info("Running TX thread (Thread ID: {})...", get_thread_id());

    // Create the table (entry timeout = 1 second)
    PingMsgMap ping_table(1);

    // Variables
    union ping_msg_t ping_msg;
    union pong_msg_t pong_msg;
    uint64_t cqe_time;
    struct ibv_wc wc = {};
    int ret;
    union rdma_addr dst_addr;

    try {
        while (true) {
            // Receive and process PING message from internal queue
            if (server_queue->try_dequeue(ping_msg)) {
                logger->debug(
                    "Internal queue received a ping_msg - pingid: {}, qpn: "
                    "{}, "
                    "gid: {}, lid: {}, ping arrival time: {}",
                    ping_msg.x.pingid, ping_msg.x.qpn,
                    parsed_gid(&ping_msg.x.gid), ping_msg.x.lid,
                    ping_msg.x.time);

                // (1) Store in table
                if (!ping_table.insert(ping_msg.x.pingid, ping_msg)) {
                    logger->warn("Failed to insert pingid {} into ping_table.",
                                 ping_msg.x.pingid);
                }

                // (2) Create and send PONG message
                pong_msg = {};
                pong_msg.x.opcode = PINGWEAVE_OPCODE_PONG;
                pong_msg.x.pingid = ping_msg.x.pingid;
                pong_msg.x.server_delay = 0;

                dst_addr.x.gid = ping_msg.x.gid;
                dst_addr.x.lid = ping_msg.x.lid;
                dst_addr.x.qpn = ping_msg.x.qpn;

                // send PONG message
                logger->debug(
                    "SEND post with PONG message of pingid: {} -> qpn: {}, "
                    "gid: {}, lid: {}",
                    pong_msg.x.pingid, dst_addr.x.qpn,
                    parsed_gid(&dst_addr.x.gid), dst_addr.x.lid);

                if (post_send(ctx_tx, dst_addr, pong_msg.raw,
                              sizeof(pong_msg_t),
                              pong_msg.x.pingid % ctx_tx->buf.size(),
                              pong_msg.x.pingid, logger)) {
                    throw std::runtime_error("SEND PONG post failed");
                }
            }

            /** NOTE: Server's TX loop must be non-blocking. */
            // Process TX CQEs
            process_tx_cqe(ctx_tx, &ping_table, logger);
        }
    } catch (const std::exception& e) {
        logger->error("Exception in TX thread: {}", e.what());
        throw;  // Propagate exception
    }
}

// RDMA server main function
void rdma_server(const std::string& ipv4) {
    // Initialize logger
    const std::string server_logname = "rdma_server_" + ipv4;
    std::shared_ptr<spdlog::logger> server_logger = initialize_custom_logger(
        server_logname, LOG_LEVEL_SERVER, LOG_FILE_SIZE, LOG_FILE_EXTRA_NUM);
    server_logger->info("RDMA Server is running on pid {}", getpid());

    // Create internal queue
    ServerInternalQueue server_queue(QUEUE_SIZE);

    // Initialize RDMA context
    pingweave_context ctx_tx, ctx_rx;
    if (initialize_contexts(ctx_tx, ctx_rx, ipv4, server_logger)) {
        throw std::runtime_error("Failed to initialize RDMA contexts.");
    }

    // Save file info for Server RX QP
    if (save_device_info(&ctx_rx, server_logger)) {
        server_logger->error("Failed to save device info: {}", ipv4);
        throw std::runtime_error("save_device_info failed.");
    }

    // Start RX thread
    std::thread rx_thread(server_rx_thread, &ctx_rx, ipv4, &server_queue,
                          server_logger);

    // Start TX thread
    std::thread tx_thread(server_tx_thread, &ctx_tx, ipv4, &server_queue,
                          server_logger);

    // termination
    if (rx_thread.joinable()) {
        rx_thread.join();
    }

    if (tx_thread.joinable()) {
        tx_thread.join();
    }
}
