#pragma once

#include "ping_info_map.hpp"
#include "ping_msg_map.hpp"
#include "rdma_common.hpp"

typedef moodycamel::ReaderWriterQueue<union ping_msg_t> ServerInternalQueue;

// Utility function: Process PONG CQE
bool process_pong_cqe(struct pingweave_context* ctx_tx,
                      std::shared_ptr<spdlog::logger> logger, struct ibv_wc& wc,
                      uint64_t cqe_time, PingMsgMap& pong_table) {
    logger->debug("-> PONG's pingID: {}", wc.wr_id);
    union ping_msg_t ping_msg = {0};

    if (pong_table.get(wc.wr_id, ping_msg)) {
        // Create ACK message
        union pong_msg_t pong_msg = {};
        pong_msg.x.opcode = PINGWEAVE_OPCODE_ACK;
        pong_msg.x.pingid = wc.wr_id;
        pong_msg.x.server_delay = calc_time_delta_with_bitwrap(
            ping_msg.x.time, cqe_time, ctx_tx->completion_timestamp_mask);
        logger->debug(
            "SEND post with ACK message pingid: {} to qpn: {}, gid: {}, lid: "
            "{}, delay: {}",
            pong_msg.x.pingid, ping_msg.x.qpn, parsed_gid(&ping_msg.x.gid),
            ping_msg.x.lid, pong_msg.x.server_delay);

        union rdma_addr dst_addr;
        dst_addr.x.gid = ping_msg.x.gid;
        dst_addr.x.lid = ping_msg.x.lid;
        dst_addr.x.qpn = ping_msg.x.qpn;

        /**
         * NOTE: Small jittering to prevent buffer override at client-side.
         * This can happen as we use RDMA UC communication.
         **/
        std::this_thread::sleep_for(std::chrono::microseconds(10));

        if (post_send(ctx_tx, dst_addr, pong_msg.raw, sizeof(pong_msg_t),
                      PINGWEAVE_WRID_SEND)) {
            if (check_log(ctx_tx->log_msg)) {
                logger->error(ctx_tx->log_msg);
            }
            return false;
        }
    } else {
        logger->warn("pingid {} entry does not exist at pong_table (expired?)",
                     wc.wr_id);
    }
    // Remove entry from table
    if (!pong_table.remove(wc.wr_id)) {
        logger->warn("Nothing to remove the id {} from pong_table", wc.wr_id);
    }
    return true;
}

// Server RX thread
void server_rx_thread(const std::string& ipv4, const std::string& logname,
                      ServerInternalQueue* server_queue,
                      struct pingweave_context* ctx_rx) {
    auto logger = spdlog::get(logname);
    logger->info("Running RX thread (Thread ID: {})...", get_thread_id());

    // Poll CQE
    struct ibv_wc wc = {};
    int ret = 0;
    uint64_t cqe_time = 0;
    union ping_msg_t ping_msg = {};
    struct ibv_poll_cq_attr attr = {};

    try {
        // Initial RECV WR posting
        if (post_recv(ctx_rx, RX_DEPTH, PINGWEAVE_WRID_RECV) != RX_DEPTH) {
            logger->error("Failed to post the next RECV WR.");
            throw std::runtime_error("Initial RECV post failed");
        }
#ifdef SERVER_RX_EVENT_DRIVEN
        /** IMPORTANT: Use event-driven polling to reduce CPU overhead */
        // Register for CQ event notifications
        if (ibv_req_notify_cq(pingweave_cq(ctx_rx), 0)) {
            logger->error("Couldn't register CQE notification");
            throw std::runtime_error("Couldn't register CQE notification");
        }
#endif
        // Polling loop
        while (true) {
#ifdef SERVER_RX_EVENT_DRIVEN
            // Wait for CQ event
            if (!wait_for_cq_event(ctx_rx, logger)) {
                throw std::runtime_error("Failed during CQ event waiting");
            }
#endif
            if (ctx_rx->rnic_hw_ts) {
                ret = ibv_start_poll(ctx_rx->cq_s.cq_ex, &attr);

                if (ret) {  // no event
#ifdef SERVER_RX_EVENT_DRIVEN
                    logger->error("ibv_start_poll failed: {}", ret);
                    throw std::runtime_error("Failed during CQ polling");
#endif
                    // small jittering to reduce CPU overhead
                    std::this_thread::sleep_for(std::chrono::microseconds(1));
                    ibv_end_poll(ctx_rx->cq_s.cq_ex);
                    continue;
                }

                // Parse PING message
                std::memcpy(&ping_msg, ctx_rx->buf + GRH_SIZE,
                            sizeof(ping_msg_t));

                // Extract WC information
                wc.opcode = ibv_wc_read_opcode(ctx_rx->cq_s.cq_ex);
                ping_msg.x.time = ibv_wc_read_completion_ts(ctx_rx->cq_s.cq_ex);
                wc.status = ctx_rx->cq_s.cq_ex->status;
                wc.wr_id = ctx_rx->cq_s.cq_ex->wr_id;

                // Post next RECV WR
                if (post_recv(ctx_rx, 1, PINGWEAVE_WRID_RECV) != 1) {
                    logger->error("Failed to post the next RECV WR.");
                    throw std::runtime_error("Failed to post next RECV WR");
                }

                // Check WC status and handle
                if (wc.status == IBV_WC_SUCCESS) {
                    if (wc.opcode == IBV_WC_RECV) {
                        logger->debug("[CQE] RECV (wr_id: {})", wc.wr_id);

                        // Parse GRH header (for debugging)
                        struct ibv_grh* grh =
                            reinterpret_cast<struct ibv_grh*>(ctx_rx->buf);
                        logger->debug("  -> from: {}", parsed_gid(&grh->sgid));

                        // For debugging
                        logger->debug(
                            "  -> id: {}, gid: {}, lid: {}, qpn: {}, time: "
                            "{}",
                            ping_msg.x.pingid, parsed_gid(&ping_msg.x.gid),
                            ping_msg.x.lid, ping_msg.x.qpn, ping_msg.x.time);

                        if (!server_queue->try_enqueue(ping_msg)) {
                            logger->error("Failed to enqueue ping message");
                            throw std::runtime_error(
                                "Failed to handle PING message");
                        }

                    } else {
                        logger->error("Unexpected opcode: {}",
                                      static_cast<int>(wc.opcode));
                        throw std::runtime_error(
                            "Unexpected opcode in RX thread");
                    }
                } else {
                    logger->error("RX WR failure - status: {}, opcode: {}",
                                  ibv_wc_status_str(wc.status),
                                  static_cast<int>(wc.opcode));
                    throw std::runtime_error("RX WR failure");
                }
                ibv_end_poll(ctx_rx->cq_s.cq_ex);

            } else {
                // poll CQE when using application-level timestamping
                ret = ibv_poll_cq(pingweave_cq(ctx_rx), 1, &wc);

                if (!ret) {  // no event
#ifdef SERVER_RX_EVENT_DRIVEN
                    logger->error("CQE poll receives nothing");
                    throw std::runtime_error("Failed during CQ polling");
#endif
                    // small jittering to reduce CPU overhead
                    std::this_thread::sleep_for(std::chrono::microseconds(1));
                    continue;
                }

                // Parse PING message
                std::memcpy(&ping_msg, ctx_rx->buf + GRH_SIZE,
                            sizeof(ping_msg_t));

                ping_msg.x.time = get_current_timestamp_steady();

                // Post next RECV WR
                if (post_recv(ctx_rx, 1, PINGWEAVE_WRID_RECV) != 1) {
                    logger->error("Failed to post the next RECV WR.");
                    throw std::runtime_error("Failed to post next RECV WR");
                }

                // Check WC status and handle
                if (wc.status == IBV_WC_SUCCESS) {
                    if (wc.opcode == IBV_WC_RECV) {
                        logger->debug("[CQE] RECV (wr_id: {})", wc.wr_id);

                        // Parse GRH header (for debugging)
                        struct ibv_grh* grh =
                            reinterpret_cast<struct ibv_grh*>(ctx_rx->buf);
                        logger->debug("  -> from: {}", parsed_gid(&grh->sgid));

                        // For debugging
                        logger->debug(
                            "  -> id: {}, gid: {}, lid: {}, qpn: {}, time: "
                            "{}",
                            ping_msg.x.pingid, parsed_gid(&ping_msg.x.gid),
                            ping_msg.x.lid, ping_msg.x.qpn, ping_msg.x.time);

                        if (!server_queue->try_enqueue(ping_msg)) {
                            logger->error("Failed to enqueue ping message");
                            throw std::runtime_error(
                                "Failed to handle PING message");
                        }

                    } else {
                        logger->error("Unexpected opcode: {}",
                                      static_cast<int>(wc.opcode));
                        throw std::runtime_error(
                            "Unexpected opcode in RX thread");
                    }
                } else {
                    logger->error("RX WR failure - status: {}, opcode: {}",
                                  ibv_wc_status_str(wc.status),
                                  static_cast<int>(wc.opcode));
                    throw std::runtime_error("RX WR failure");
                }
            }
        }
    } catch (const std::exception& e) {
        logger->error("RX thread exits unexpectedly: {}", e.what());
        throw;  // Propagate exception
    }
}

void server_tx_thread(const std::string& ipv4, const std::string& logname,
                      ServerInternalQueue* server_queue,
                      struct pingweave_context* ctx_tx) {
    auto logger = spdlog::get(logname);

    // TX thread loop - handle messages
    logger->info("Running TX thread (Thread ID: {})...", get_thread_id());

    // Create the table (entry timeout = 1 second)
    PingMsgMap pong_table(1);

    // Variables
    union ping_msg_t ping_msg;
    union pong_msg_t pong_msg;
    uint64_t cqe_time;
    struct ibv_wc wc = {};
    int ret;
    union rdma_addr dst_addr;
    struct ibv_poll_cq_attr attr = {};

    while (true) {
        // Receive and process PING message from internal queue
        if (server_queue->try_dequeue(ping_msg)) {
            logger->debug(
                "Internal queue received a ping_msg - pingid: {}, qpn: "
                "{}, "
                "gid: {}, lid: {}, ping arrival time: {}",
                ping_msg.x.pingid, ping_msg.x.qpn, parsed_gid(&ping_msg.x.gid),
                ping_msg.x.lid, ping_msg.x.time);

            // (1) Store in table
            if (!pong_table.insert(ping_msg.x.pingid, ping_msg)) {
                logger->warn("Failed to insert pingid {} into pong_table.",
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
                pong_msg.x.pingid, dst_addr.x.qpn, parsed_gid(&dst_addr.x.gid),
                dst_addr.x.lid);
            if (post_send(ctx_tx, dst_addr, pong_msg.raw, sizeof(pong_msg_t),
                          pong_msg.x.pingid)) {
                if (check_log(ctx_tx->log_msg)) {
                    logger->error(ctx_tx->log_msg);
                }
                throw std::runtime_error("SEND PONG post failed");
            }
        }

        /**
         * IMPORTANT: Use non-blocking polling.
         * Otherwise, getting msg from server_queue will be blocked.
         */
        // Capture and process CQE
        if (ctx_tx->rnic_hw_ts) {
            // Poll CQE when using RNIC timestamping
            ret = ibv_start_poll(ctx_tx->cq_s.cq_ex, &attr);
            bool has_events = false;

            if (ret) {  // no event
                // to minize CPU overhead for polling
                std::this_thread::sleep_for(std::chrono::microseconds(10));
                continue;
            }

            while (!ret) {
                has_events = true;

                // Extract WC information
                wc.status = ctx_tx->cq_s.cq_ex->status;
                wc.wr_id = ctx_tx->cq_s.cq_ex->wr_id;
                wc.opcode = ibv_wc_read_opcode(ctx_tx->cq_s.cq_ex);
                cqe_time = ibv_wc_read_completion_ts(ctx_tx->cq_s.cq_ex);

                // Check WC status and handle
                if (wc.status == IBV_WC_SUCCESS) {
                    if (wc.opcode == IBV_WC_SEND) {
                        if (wc.wr_id == PINGWEAVE_WRID_SEND) {
                            logger->debug("CQE of ACK. Do nothing.");
                        } else {
                            if (process_pong_cqe(ctx_tx, logger, wc, cqe_time,
                                                 pong_table)) {
                                // Successfully processed
                            } else {
                                throw std::runtime_error(
                                    "Failed to process PONG CQE");
                            }
                        }
                    } else {
                        logger->error("Unexpected opcode: {}",
                                      static_cast<int>(wc.opcode));
                        throw std::runtime_error("Unexpected opcode");
                    }
                } else {
                    logger->warn("TX WR failure - status: {}, opcode: {}",
                                 ibv_wc_status_str(wc.status),
                                 static_cast<int>(wc.opcode));
                    throw std::runtime_error("TX WR failure");
                }

                // Poll next event
                ret = ibv_next_poll(ctx_tx->cq_s.cq_ex);
            }
            if (has_events) {
                ibv_end_poll(ctx_tx->cq_s.cq_ex);
            }
        } else {
            // Poll CQE when using application-level timestamping
            ret = ibv_poll_cq(pingweave_cq(ctx_tx), 1, &wc);

            if (!ret) {  // no event
                // to minize CPU overhead for polling
                std::this_thread::sleep_for(std::chrono::microseconds(10));
                continue;
            }

            while (ret) {
                cqe_time = get_current_timestamp_steady();
                if (wc.status == IBV_WC_SUCCESS) {
                    if (wc.opcode == IBV_WC_SEND) {
                        if (wc.wr_id == PINGWEAVE_WRID_SEND) {
                            logger->debug("CQE of ACK. Do nothing.");
                        } else {
                            if (process_pong_cqe(ctx_tx, logger, wc, cqe_time,
                                                 pong_table)) {
                                // Successfully processed
                            } else {
                                throw std::runtime_error(
                                    "Failed to process PONG CQE");
                            }
                        }
                    } else {
                        logger->error("Unexpected opcode: {}",
                                      static_cast<int>(wc.opcode));
                        throw std::runtime_error("Unexpected opcode");
                    }
                } else {
                    logger->warn("TX WR failure - status: {}, opcode: {}",
                                 ibv_wc_status_str(wc.status),
                                 static_cast<int>(wc.opcode));
                    throw std::runtime_error("TX WR failure");
                }

                // Poll next event
                ret = ibv_poll_cq(pingweave_cq(ctx_tx), 1, &wc);
            }
        }
    }
}

// RDMA server main function
void rdma_server(const std::string& ipv4) {
    // Initialize logger
    const std::string server_logname = "rdma_server_" + ipv4;
    auto server_logger = initialize_custom_logger(
        server_logname, LOG_LEVEL_SERVER, LOG_FILE_SIZE, LOG_FILE_EXTRA_NUM);
    server_logger->info("RDMA Server is running on pid {}", getpid());

    // Create internal queue
    ServerInternalQueue server_queue(QUEUE_SIZE);

    // Initialize RDMA context
    struct pingweave_context ctx_tx, ctx_rx;
    if (make_ctx(&ctx_tx, ipv4, server_logger, false)) {
        server_logger->error("Failed to make TX device info: {}", ipv4);
        throw std::runtime_error("make_ctx failed.");
    }

    if (make_ctx(&ctx_rx, ipv4, server_logger, true)) {
        server_logger->error("Failed to make RX device info: {}", ipv4);
        throw std::runtime_error("make_ctx failed.");
    }
    if (save_device_info(&ctx_rx, server_logger)) {
        server_logger->error("Failed to save device info: {}", ipv4);
        throw std::runtime_error("save_device_info failed.");
    }

    // Start RX thread
    std::thread rx_thread(server_rx_thread, ipv4, server_logname, &server_queue,
                          &ctx_rx);

    // Start TX thread
    std::thread tx_thread(server_tx_thread, ipv4, server_logname, &server_queue,
                          &ctx_tx);

    // termination
    if (rx_thread.joinable()) {
        rx_thread.join();
    }

    if (tx_thread.joinable()) {
        tx_thread.join();
    }
}
