#pragma once

#include "ping_info_map.hpp"
#include "ping_msg_map.hpp"
#include "rdma_common.hpp"
#include "rdma_scheduler.hpp"

// Function to handle received messages
void handle_received_message(pingweave_context* ctx_rx,
                             const union pong_msg_t& pong_msg,
                             PingInfoMap* ping_table, const uint64_t& recv_time,
                             const uint64_t& cqe_time,
                             std::shared_ptr<spdlog::logger> logger) {
    if (pong_msg.x.opcode == PINGWEAVE_OPCODE_PONG) {
        // Handle PONG message
        logger->debug("[CQE] -> Recv PONG ({}): recv_time {}, cqe_time:{}",
                      pong_msg.x.pingid, recv_time, cqe_time);
        if (!ping_table->update_pong_info(pong_msg.x.pingid, recv_time,
                                          UINT64_MAX, cqe_time,
                                          ctx_rx->completion_timestamp_mask)) {
            logger->warn("PONG ({}): No entry in ping_table.",
                         pong_msg.x.pingid);
        }

    } else if (pong_msg.x.opcode == PINGWEAVE_OPCODE_ACK) {
        // Handle ACK message
        logger->debug("[CQE] -> Recv PONG_ACK ({}): server_delay {}",
                      pong_msg.x.pingid, pong_msg.x.server_delay);
        if (!ping_table->update_ack_info(pong_msg.x.pingid,
                                         pong_msg.x.server_delay)) {
            logger->warn("PONG_ACK ({}): No entry in ping_table.",
                         pong_msg.x.pingid);
        }
    } else {
        logger->error("Unknown opcode received: {}", pong_msg.x.opcode);
    }
}

// Function to process RX CQEs
void client_process_rx_cqe(pingweave_context* ctx_rx, PingInfoMap* ping_table,
                           std::shared_ptr<spdlog::logger> logger) {
    uint64_t cqe_time = 0, recv_time = 0;
    struct ibv_wc wc = {};
    int ret = 0;
    union pong_msg_t pong_msg = {};
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
                    ret = ibv_next_poll(ctx_rx->cq_s.cq_ex);
                    continue;
                }

                wc.wr_id = ctx_rx->cq_s.cq_ex->wr_id;
                wc.opcode = ibv_wc_read_opcode(ctx_rx->cq_s.cq_ex);
                cqe_time = ibv_wc_read_completion_ts(ctx_rx->cq_s.cq_ex);

                if (wc.opcode == IBV_WC_RECV) {
                    logger->debug("[CQE] RECV (wr_id: {})", wc.wr_id);

                    // Parse the received message
                    auto buf = ctx_rx->buf[wc.wr_id];
                    std::memcpy(&pong_msg, buf.addr + GRH_SIZE,
                                sizeof(pong_msg_t));

                    // Post the next RECV WR
                    if (post_recv(ctx_rx, wc.wr_id, 1) == 0) {
                        logger->warn("Failed to repost the next RECV WR.");
                    }

                    // Get current time
                    recv_time = get_current_timestamp_steady();

                    // Handle the received message (PONG or ACK)
                    handle_received_message(ctx_rx, pong_msg, ping_table,
                                            recv_time, cqe_time, logger);
                } else {
                    logger->error("Unexpected opcode: {}",
                                  static_cast<int>(wc.opcode));
                    throw std::runtime_error("Unexpected opcode");
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
            num_cqes = ibv_poll_cq(pingweave_cq(ctx_rx), BATCH_CQE, wc_array);

            if (num_cqes < 0) {
                throw std::runtime_error("Failed to poll CQ");
            } else if (num_cqes == 0) {  // no completion
                throw std::runtime_error("Failed during CQ polling");
            }

            for (int i = 0; i < num_cqes; ++i) {
                struct ibv_wc& wc = wc_array[i];

                // if failure
                if (wc.status != IBV_WC_SUCCESS) {
                    logger->error("CQE RX error: {}",
                                  ibv_wc_status_str(wc.status));
                    continue;
                }

                if (wc.opcode == IBV_WC_RECV) {
                    logger->debug("[CQE] RECV (wr_id: {})", wc.wr_id);

                    // Get current time
                    cqe_time = get_current_timestamp_steady();
                    recv_time = cqe_time;

                    // Parse the received message
                    auto buf = ctx_rx->buf[wc.wr_id];
                    std::memcpy(&pong_msg, buf.addr + GRH_SIZE,
                                sizeof(pong_msg_t));

                    // Post the next RECV WR
                    if (post_recv(ctx_rx, wc.wr_id, 1) == 0) {
                        logger->warn("Failed to repost the next RECV WR.");
                    }

                    // Handle the received message (PONG or ACK)
                    handle_received_message(ctx_rx, pong_msg, ping_table,
                                            recv_time, cqe_time, logger);
                } else {
                    logger->error("[CQE] RX WR - status: {}, opcode: {}",
                                  ibv_wc_status_str(wc.status),
                                  static_cast<int>(wc.opcode));
                    throw std::runtime_error("RX WR failure");
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

// Function to process TX CQEs
void client_process_tx_cqe(pingweave_context* ctx_tx, PingInfoMap* ping_table,
                           std::shared_ptr<spdlog::logger> logger) {
    uint64_t cqe_time = 0;
    struct ibv_wc wc = {};
    int ret = 0;

    /**
     * IMPORTANT: Use non-blocking polling.
     * Otherwise, scheduling the next message to send will be blocked.
     */
    try {
        if (ctx_tx->rnic_hw_ts) {
            // Use extended CQ polling for hardware timestamping
            struct ibv_poll_cq_attr attr = {};
            ret = ibv_start_poll(ctx_tx->cq_s.cq_ex, &attr);
            bool has_events = false;

            while (!ret) {
                has_events = true;

                // Process the current CQE
                wc.status = ctx_tx->cq_s.cq_ex->status;

                // if failure
                if (wc.status != IBV_WC_SUCCESS) {
                    logger->error("CQE TX error: {}",
                                  ibv_wc_status_str(wc.status));
                    ret = ibv_next_poll(ctx_tx->cq_s.cq_ex);
                    continue;
                }

                wc.wr_id = ctx_tx->cq_s.cq_ex->wr_id;
                wc.opcode = ibv_wc_read_opcode(ctx_tx->cq_s.cq_ex);
                cqe_time = ibv_wc_read_completion_ts(ctx_tx->cq_s.cq_ex);

                if (wc.opcode == IBV_WC_SEND) {
                    logger->debug(
                        "[CQE] Send complete (ping ID: {}), time: {}.",
                        wc.wr_id, cqe_time);
                    if (!ping_table->update_ping_cqe_time(wc.wr_id, cqe_time)) {
                        logger->warn(
                            "Failed to update send completion time for ping ID "
                            "{}.",
                            wc.wr_id);
                    }
                } else {
                    logger->error("[CQE] TX WR - status: {}, opcode: {}",
                                  ibv_wc_status_str(wc.status),
                                  static_cast<int>(wc.opcode));
                    throw std::runtime_error("TX WR failure");
                }

                // poll next event
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
            int num_cqes =
                ibv_poll_cq(pingweave_cq(ctx_tx), BATCH_CQE, wc_array);
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
                    logger->error("CQE TX error: {}",
                                  ibv_wc_status_str(wc.status));
                    continue;
                }

                if (wc.opcode == IBV_WC_SEND) {
                    logger->debug(
                        "[CQE] SEND completed (ping ID: {}), time: {}",
                        wc.wr_id, cqe_time);
                } else {
                    logger->error("[CQE] TX WR - status: {}, opcode: {}",
                                  ibv_wc_status_str(wc.status),
                                  static_cast<int>(wc.opcode));
                    throw std::runtime_error("TX WR failure");
                }
            }
        }
    } catch (const std::exception& e) {
        logger->error("TX CQE handler exits unexpectedly: {}", e.what());
        throw;  // Propagate exception
    }

    return;
}

void client_rx_thread(struct pingweave_context* ctx_rx, const std::string& ipv4,
                      PingInfoMap* ping_table,
                      std::shared_ptr<spdlog::logger> logger) {
    logger->info("Running RX thread (Thread ID: {})...", get_thread_id());

    // RECV WR uses wr_id as a buffer index
    for (int i = 0; i < ctx_rx->buf.size(); ++i) {
        if (post_recv(ctx_rx, i, RX_DEPTH) != RX_DEPTH) {
            logger->error("Failed to post RECV WRs when initialization.");
            exit(EXIT_FAILURE);
        }
    }

    /** IMPORTANT: Use event-driven polling to reduce CPU overhead */
    // Register for CQ event notifications
    if (ibv_req_notify_cq(pingweave_cq(ctx_rx), 0)) {
        logger->error("Couldn't register CQE notification");
        exit(EXIT_FAILURE);
    }

    // Start the receive loop
    try {
        while (true) {
            // Wait for the next CQE
            if (!wait_for_cq_event(ctx_rx, logger)) {
                logger->error("Failed during CQ event waiting");
                throw std::runtime_error("Failed during CQ event waiting");
            }

            // Process RX CQEs
            client_process_rx_cqe(ctx_rx, ping_table, logger);
        }
    } catch (const std::exception& e) {
        logger->error("Exception in RX thread: {}", e.what());
        exit(EXIT_FAILURE);
    }
}

void client_tx_thread(struct pingweave_context* ctx_tx, const std::string& ipv4,
                      PingInfoMap* ping_table, const union ibv_gid& rx_gid,
                      const uint32_t& rx_lid, const uint32_t& rx_qpn,
                      std::shared_ptr<spdlog::logger> logger) {
    logger->info("Running TX thread (Thead ID: {})...", get_thread_id());

    uint32_t ping_uid = 0;
    MsgScheduler scheduler(ipv4, logger);
    std::tuple<std::string, std::string, uint32_t, uint32_t> dst_info;

    try {
        while (true) {
            // Retrieve the next destination for sending
            if (scheduler.next(dst_info)) {
                const auto& [dst_ip, dst_gid_str, dst_lid, dst_qpn] = dst_info;

                // Set the destination address
                union rdma_addr dst_addr = {};
                dst_addr.x.qpn = dst_qpn;
                wire_gid_to_gid(dst_gid_str.c_str(), &dst_addr.x.gid);
                dst_addr.x.lid = dst_lid;

                // Create the PING message
                union ping_msg_t msg = {};
                msg.x.pingid = make_pingid(ip2uint(ipv4), ping_uid++);
                msg.x.qpn = rx_qpn;
                msg.x.gid = rx_gid;
                msg.x.lid = rx_lid;

                // Record the send time
                uint64_t send_time_system = get_current_timestamp_ns();
                uint64_t send_time_steady = get_current_timestamp_steady();
                if (!ping_table->insert(
                        msg.x.pingid,
                        {msg.x.pingid, msg.x.qpn, msg.x.gid, msg.x.lid, dst_ip,
                         send_time_system, send_time_steady, 0, 0,
                         PINGWEAVE_MASK_INIT})) {
                    logger->warn("Failed to insert ping ID {} into ping_table.",
                                 msg.x.pingid);
                }

                // Send the PING message
                logger->debug(
                    "Sending PING message (ping ID: {}, QPN: {}, GID: {}, LID: "
                    "{}), time: {}, dst_GID:{}",
                    msg.x.pingid, msg.x.qpn, parsed_gid(&msg.x.gid), msg.x.lid,
                    send_time_steady, parsed_gid(&dst_addr.x.gid));

                // sanity check
                assert(msg.x.pingid > PINGWEAVE_WRID_PONG_ACK);

                if (post_send(ctx_tx, dst_addr, msg.raw, sizeof(ping_msg_t),
                              msg.x.pingid % ctx_tx->buf.size(), msg.x.pingid,
                              logger)) {
                    logger->error("Failed to send PING message, dst_ip: {}.",
                                  dst_ip);
                }
                // Get current time
                auto cqe_time = get_current_timestamp_steady();
                if (!ping_table->update_ping_cqe_time(msg.x.pingid, cqe_time)) {
                    logger->warn(
                        "Failed to update send completion time for ping ID "
                        "{}.",
                        msg.x.pingid);
                }
            }

            /** NOTE: Client's TX loop must be non-blocking. */
            // Process TX CQEs
            client_process_tx_cqe(ctx_tx, ping_table, logger);
        }
    } catch (const std::exception& e) {
        logger->error("Exception in TX thread: {}", e.what());
        throw;  // Propagate exception
    }
}

void client_result_thread(const std::string& ipv4,
                          ClientInternalQueue* client_queue,
                          std::shared_ptr<spdlog::logger> logger) {
    // dstip -> result history
    std::unordered_map<uint32_t, struct result_info_t> dstip2result;

    // msg from RX Thread
    struct result_msg_t result_msg;

    // result pointer
    struct result_info_t* info;

    // timer for report
    auto last_report_time = std::chrono::steady_clock::now();

    /** RESULT: (dstip, #success, #failure, mean, max, p50, p95, p99) */
    try {
        while (true) {
            // fully-blocking with timeout (1 sec)
            if (client_queue->wait_dequeue_timed(
                    result_msg, std::chrono::seconds(WAIT_DEQUEUE_TIME_SEC))) {
                logger->debug("{}, {}, {}, {}, {}, {}, {}",
                              timestamp_ns_to_string(result_msg.time_ping_send),
                              uint2ip(result_msg.dstip), result_msg.pingid,
                              result_msg.client_delay, result_msg.network_delay,
                              result_msg.server_delay, result_msg.success);
                // load a result
                info = &dstip2result[result_msg.dstip];
                if (info->ts_start == 0) {
                    info->ts_start = result_msg.time_ping_send;  // initialize
                }
                info->ts_end = result_msg.time_ping_send;

                if (result_msg.success) {  // success
                    ++info->n_success;
                    info->client_delays.push_back(result_msg.client_delay);
                    info->network_delays.push_back(result_msg.network_delay);
                    info->server_delays.push_back(result_msg.server_delay);
                } else {  // failure
                    ++info->n_failure;
                }
            }

            // Check the interval for report
            auto current_time = std::chrono::steady_clock::now();
            auto elapsed_time =
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    current_time - last_report_time)
                    .count();

            if (elapsed_time >= REPORT_INTERVAL_MS) {
                // aggregated results
                std::string agg_result = "";

                for (auto& [dstip, result_info] : dstip2result) {
                    result_stat_t client_stat =
                        calc_stats(result_info.client_delays);
                    result_stat_t network_stat =
                        calc_stats(result_info.network_delays);
                    result_stat_t server_stat =
                        calc_stats(result_info.server_delays);

                    auto result = convert_result_to_str(
                        ipv4, uint2ip(dstip), result_info, client_stat,
                        network_stat, server_stat);
                    logger->info(result);         // logging
                    agg_result += result + "\n";  // aggregate logs
                }

                // send to collector
                if (agg_result.size() > 0) {
                    message_to_http_server(agg_result, "/result_rdma", logger);
                }

                // clear the history
                dstip2result.clear();

                // update the last report time
                last_report_time = current_time;
            }
        }
    } catch (const std::exception& e) {
        logger->error("Exception in result thread: {}", e.what());
    }
}

void rdma_client(const std::string& ipv4) {
    // Start the RX thread
    const std::string client_logname = "rdma_client_" + ipv4;
    std::shared_ptr<spdlog::logger> client_logger =
        initialize_logger(client_logname, DIR_LOG_PATH, LOG_LEVEL_CLIENT,
                          LOG_FILE_SIZE, LOG_FILE_EXTRA_NUM);
    client_logger->info("RDMA Client is running on pid {}", getpid());

    // Inter-thread queue
    const std::string result_logname = "rdma_" + ipv4;
    std::shared_ptr<spdlog::logger> result_logger =
        initialize_logger(result_logname, DIR_RESULT_PATH, LOG_LEVEL_RESULT,
                          LOG_FILE_SIZE, LOG_FILE_EXTRA_NUM);
    ClientInternalQueue client_queue(QUEUE_SIZE);

    // ping table with timeout
    const std::string ping_table_logname = "rdma_table_" + ipv4;
    std::shared_ptr<spdlog::logger> ping_table_logger = initialize_logger(
        ping_table_logname, DIR_LOG_PATH, LOG_LEVEL_PING_TABLE, LOG_FILE_SIZE,
        LOG_FILE_EXTRA_NUM);
    PingInfoMap ping_table(ping_table_logger, &client_queue, 1000);

    // Initialize RDMA contexts
    pingweave_context ctx_tx, ctx_rx;
    if (initialize_contexts(ctx_tx, ctx_rx, ipv4, client_logger)) {
        throw std::runtime_error("Failed to initialize RDMA contexts.");
    }

    // Start the Result thread
    client_logger->info("Starting result thread (Thread ID: {})...",
                        get_thread_id());
    std::thread result_thread(client_result_thread, ipv4, &client_queue,
                              result_logger);

    // Start the RX thread
    std::thread rx_thread(client_rx_thread, &ctx_rx, ipv4, &ping_table,
                          client_logger);

    // Start the TX thread
    std::thread tx_thread(client_tx_thread, &ctx_tx, ipv4, &ping_table,
                          ctx_rx.gid, ctx_rx.portinfo.lid, ctx_rx.qp->qp_num,
                          client_logger);

    // termination
    if (tx_thread.joinable()) {
        tx_thread.join();
    }

    if (result_thread.joinable()) {
        result_thread.join();
    }
}