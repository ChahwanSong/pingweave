#pragma once

#include "rdma_common.hpp"
#include "rdma_ping_info.hpp"
#include "rdma_ping_msg.hpp"
#include "rdma_scheduler.hpp"

// BUG FIX: IB CQE timestamp fluctuates ~ 2**33
std::atomic<uint64_t> client_archive_cqe_hw_clock = 0;

// Function to handle received messages
void handle_received_message(rdma_context* ctx_rx,
                             const union rdma_pongmsg_t& pong_msg,
                             RdmaPinginfoMap* ping_table,
                             const uint64_t& recv_time,
                             const uint64_t& cqe_time,
                             std::shared_ptr<spdlog::logger> logger) {
    if (pong_msg.x.opcode == PINGWEAVE_OPCODE_PONG) {
        // Handle PONG message
        logger->debug("[CQE] -> Recv PONG ({}): recv_time: {}, cqe_time:{}",
                      pong_msg.x.pingid, recv_time, cqe_time);
        if (!ping_table->update_pong_info(pong_msg.x.pingid, recv_time,
                                          cqe_time)) {
            logger->debug("PONG ({}): No entry in ping_table.",
                          pong_msg.x.pingid);
        }
    } else if (pong_msg.x.opcode == PINGWEAVE_OPCODE_ACK) {
        // Handle ACK message
        logger->debug("[CQE] -> Recv PONG_ACK ({}): server_delay: {}",
                      pong_msg.x.pingid, pong_msg.x.server_delay);
        if (!ping_table->update_ack_info(pong_msg.x.pingid,
                                         pong_msg.x.server_delay)) {
            logger->debug("PONG_ACK ({}): No entry in ping_table.",
                          pong_msg.x.pingid);
        }
    } else {
        logger->error("Unknown opcode received: {}", pong_msg.x.opcode);
    }
}

// Function to process RX CQEs
void client_process_rx_cqe(rdma_context* ctx_rx, RdmaPinginfoMap* ping_table,
                           std::shared_ptr<spdlog::logger> logger) {
    uint64_t cqe_time = 0, recv_time = 0;
    struct ibv_wc wc = {};
    int ret = 0;
    union rdma_pongmsg_t pong_msg = {};
    int num_cqes = 0;

    try {
        if (ctx_rx->rnic_hw_ts) {
            // Use extended CQ polling for hardware timestamping
            struct ibv_poll_cq_attr attr = {};
            bool has_events = false;
            ret = ibv_start_poll(ctx_rx->cq_s.cq_ex, &attr);

            while (!ret) {
                ++num_cqes;
                logger->trace("CQ Event loop {}", num_cqes);
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

                // /** HW TIMESTAMP JUMP CORRECTION LOGIC */
                // if (client_archive_cqe_hw_clock.load() < cqe_time) {
                //     client_archive_cqe_hw_clock.store(cqe_time);
                // } else {
                //     /**
                //      * In case of Infiniband, HW timestamp sometimes
                //      fluctuates
                //      * like 8589934592 (2**33). We try to adjust it.
                //      */
                //     auto adjusted_cqe_time =
                //         cqe_time + PINGWEAVE_IB_HW_ADJUST_TIME;
                //     logger->debug(
                //         "rx cqe - original cqe_time: {}, adjusted: {}",
                //         cqe_time, adjusted_cqe_time);
                //     client_archive_cqe_hw_clock.store(adjusted_cqe_time);
                //     cqe_time = adjusted_cqe_time;
                // }
                // /*--------------------------------------*/

                if (wc.opcode == IBV_WC_RECV) {
                    // Parse the received message
                    auto buf = ctx_rx->buf[wc.wr_id];
                    std::memcpy(&pong_msg, buf.addr + GRH_SIZE,
                                sizeof(rdma_pongmsg_t));

                    logger->debug("[CQE] RECV (ping ID: {}, wr_id: {})",
                                  pong_msg.x.pingid, wc.wr_id);

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
                    throw std::runtime_error("rx cqe - Unexpected opcode");
                }

                ret = ibv_next_poll(ctx_rx->cq_s.cq_ex);
            }

            if (has_events) {
                // End the polling session
                ibv_end_poll(ctx_rx->cq_s.cq_ex);
            } else {  // nothing to poll
                logger->error("RX: CQE poll receives nothing");
                throw std::runtime_error("rx cqe - Failed during CQ polling");
            }
        } else {
            struct ibv_wc wc_array[BATCH_CQE];
            num_cqes = ibv_poll_cq(pingweave_cq(ctx_rx), BATCH_CQE, wc_array);

            if (num_cqes < 0) {
                throw std::runtime_error("rx cqe - Failed to poll CQ");
            } else if (num_cqes == 0) {  // no completion
                throw std::runtime_error("rx cqe - Failed during CQ polling");
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
                                sizeof(rdma_pongmsg_t));

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
                    throw std::runtime_error("rx cqe - RX WR failure");
                }
            }
        }

        // Acknowledge the CQ event
        ibv_ack_cq_events(pingweave_cq(ctx_rx), num_cqes);

        // Re-register for CQ event notifications
        if (ibv_req_notify_cq(pingweave_cq(ctx_rx), 0)) {
            logger->error("Couldn't register CQE notification");
            throw std::runtime_error(
                "rx cqe - Failed to post cqe request notification.");
        }

    } catch (const std::exception& e) {
        logger->error("RX CQE handler exits unexpectedly: {}", e.what());
        throw;  // Propagate exception
    }
}

// Function to process TX CQEs
void client_process_tx_cqe(rdma_context* ctx_tx, RdmaPinginfoMap* ping_table,
                           std::shared_ptr<spdlog::logger> logger) {
    uint64_t cqe_time = 0;
    struct ibv_wc wc = {};
    int ret = 0;
    int num_cqes = 0;

    /**
     * IMPORTANT: Use non-blocking polling.
     * Otherwise, scheduling the next message to send will be blocked.
     */
    try {
        if (ctx_tx->rnic_hw_ts) {
            // Use extended CQ polling for hardware timestamping
            struct ibv_poll_cq_attr attr = {};
            bool has_events = false;
            ret = ibv_start_poll(ctx_tx->cq_s.cq_ex, &attr);

            while (!ret) {
                ++num_cqes;
                logger->trace("CQ Event loop {}", num_cqes);
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

                // /** HW TIMESTAMP JUMP CORRECTION LOGIC */
                // if (client_archive_cqe_hw_clock.load() < cqe_time) {
                //     client_archive_cqe_hw_clock.store(cqe_time);
                // } else {
                //     /**
                //      * In case of Infiniband, HW timestamp sometimes
                //      fluctuates
                //      * like 8589934592 (2**33). We try to adjust it.
                //      */
                //     auto adjusted_cqe_time =
                //         cqe_time + PINGWEAVE_IB_HW_ADJUST_TIME;
                //     logger->debug(
                //         "tx cqe - pingid: {}, original cqe_time: {},
                //         adjusted: "
                //         "{}",
                //         wc.wr_id, cqe_time, adjusted_cqe_time);
                //     client_archive_cqe_hw_clock.store(adjusted_cqe_time);
                //     cqe_time = adjusted_cqe_time;
                // }
                // /*--------------------------------------*/

                if (wc.opcode == IBV_WC_SEND) {
                    logger->debug(
                        "[CQE] Send complete (ping ID: {}), cqe_time: {}.",
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
                    throw std::runtime_error("tx cqe - TX WR failure");
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
            num_cqes = ibv_poll_cq(pingweave_cq(ctx_tx), BATCH_CQE, wc_array);
            if (num_cqes < 0) {
                logger->error("Failed to poll CQ");
                throw std::runtime_error("tx cqe - Failed to poll CQ");
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
                    throw std::runtime_error("tx cqe - TX WR failure");
                }
            }
        }

        // Acknowledge the CQ event
        ibv_ack_cq_events(pingweave_cq(ctx_tx), num_cqes);

        // Re-register for CQ event notifications
        if (ibv_req_notify_cq(pingweave_cq(ctx_tx), 0)) {
            logger->error("Couldn't register CQE notification");
            throw std::runtime_error(
                "tx cqe - Failed to post cqe request notification.");
        }

    } catch (const std::exception& e) {
        logger->error("TX CQE handler exits unexpectedly: {}", e.what());
        throw;  // Propagate exception
    }

    return;
}

void rdma_client_rx_thread(struct rdma_context* ctx_rx, const std::string& ipv4,
                           RdmaPinginfoMap* ping_table,
                           std::shared_ptr<spdlog::logger> logger) {
    logger->info("Running RX thread (Thread ID: {})...", get_thread_id());

    // RECV WR uses wr_id as a buffer index
    for (int i = 0; i < ctx_rx->buf.size(); ++i) {
        if (post_recv(ctx_rx, i, RX_DEPTH) != RX_DEPTH) {
            logger->error("Failed to post RECV WRs when initialization.");
            throw;  // Propagate exception
        }
    }

    /** IMPORTANT: Use event-driven polling to reduce CPU overhead */
    // Register for CQ event notifications
    if (ibv_req_notify_cq(pingweave_cq(ctx_rx), 0)) {
        logger->error("Couldn't register CQE notification");
        throw;  // Propagate exception`
    }

    // Start the receive loop
    try {
        while (true) {
            // Wait for the next CQE
            if (wait_for_cq_event(ctx_rx, logger)) {
                logger->error("Failed during CQ event waiting");
                throw std::runtime_error(
                    "rx thread - Failed during CQ event waiting");
            }

            // Process RX CQEs
            client_process_rx_cqe(ctx_rx, ping_table, logger);
        }
    } catch (const std::exception& e) {
        logger->error("Exception in RX thread: {}", e.what());
        throw;  // Propagate exception`
    }
}

void rdma_client_tx_sched_thread(struct rdma_context* ctx_tx,
                                 const std::string& ipv4,
                                 RdmaPinginfoMap* ping_table,
                                 const union ibv_gid& rx_gid,
                                 const uint32_t& rx_lid, const uint32_t& rx_qpn,
                                 std::shared_ptr<spdlog::logger> logger) {
    logger->info("Running TX Scheduler thread (Thead ID: {})...",
                 get_thread_id());

    uint32_t ping_uid = 0;
    uint64_t time_sleep_us = 0;
    RdmaMsgScheduler scheduler(ipv4, logger);
    std::tuple<std::string, std::string, uint32_t, uint32_t> dst_addr;

    try {
        while (true) {
            // Retrieve the next destination for sending
            if (scheduler.next(dst_addr, time_sleep_us)) {
                const auto& [dst_ip, dst_gid_str, dst_lid, dst_qpn] = dst_addr;

                // Set the destination address
                union rdma_addr dst_addr = {};
                dst_addr.x.qpn = dst_qpn;
                wire_gid_to_gid(dst_gid_str.c_str(), &dst_addr.x.gid);
                dst_addr.x.lid = dst_lid;

                // Create the PING message
                union rdma_pingmsg_t msg = {};
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
                    "{}), Timestamp: {}, send_time: {}, dst_GID:{}",
                    msg.x.pingid, msg.x.qpn, parsed_gid(&msg.x.gid), msg.x.lid,
                    timestamp_ns_to_string(send_time_system), send_time_steady,
                    parsed_gid(&dst_addr.x.gid));

                // sanity check
                assert(msg.x.pingid > PINGWEAVE_WRID_PONG_ACK);

                if (post_send(ctx_tx, dst_addr, msg.raw, sizeof(rdma_pingmsg_t),
                              msg.x.pingid % ctx_tx->buf.size(), msg.x.pingid,
                              logger)) {
                    logger->error("Failed to send PING message, dst: {}.",
                                  dst_ip);
                } else {
                    // if RNIC_TIMESTAMP is not supported, update ping cqe here
                    if (!ctx_tx->rnic_hw_ts) {
                        auto cqe_time = send_time_steady;
                        if (!ping_table->update_ping_cqe_time(msg.x.pingid,
                                                              cqe_time)) {
                            logger->warn(
                                "Failed to update send completion time for "
                                "ping ID {}.",
                                msg.x.pingid);
                        }
                    }
                }
            } else {
                // sleep until next ping schedule
                std::this_thread::sleep_for(
                    std::chrono::microseconds(time_sleep_us));
            }
        }
    } catch (const std::exception& e) {
        logger->error("Exception in TX thread: {}", e.what());
        throw;  // Propagate exception
    }
}

void rdma_client_tx_cqe_thread(struct rdma_context* ctx_tx,
                               const std::string& ipv4,
                               RdmaPinginfoMap* ping_table,
                               const union ibv_gid& rx_gid,
                               const uint32_t& rx_lid, const uint32_t& rx_qpn,
                               std::shared_ptr<spdlog::logger> logger) {
    logger->info("Running TX CQE thread (Thead ID: {})...", get_thread_id());

    /** IMPORTANT: Use event-driven polling to reduce CPU overhead */
    // Register for CQ event notifications
    if (ibv_req_notify_cq(pingweave_cq(ctx_tx), 0)) {
        logger->error("Couldn't register CQE notification");
        throw;  // Propagate exception`
    }

    // Start the receive loop
    try {
        while (true) {
            // Wait for the next CQE
            if (wait_for_cq_event(ctx_tx, logger)) {
                logger->error("Failed during CQ event waiting");
                throw std::runtime_error(
                    "tx cqe - Failed during CQ event waiting");
            }

            // Process TX CQEs
            client_process_tx_cqe(ctx_tx, ping_table, logger);
        }
    } catch (const std::exception& e) {
        logger->error("Exception in TX thread: {}", e.what());
        throw;  // Propagate exception`
    }
}

void rdma_client_result_thread(const std::string& ipv4,
                               RdmaClientQueue* client_queue,
                               std::shared_ptr<spdlog::logger> logger) {
    int report_interval_ms = 10000;
    if (!get_int_param_from_ini(report_interval_ms,
                                "interval_report_ping_result_millisec")) {
        logger->error(
            "Failed to load 'report_interval' from pingwewave.ini. Use default "
            "- 10 sec.");
        report_interval_ms = 10000;
    }

    // dstip -> result history
    std::unordered_map<uint32_t, struct rdma_result_info_t> dstip2result;

    // msg from RX Thread
    struct rdma_result_msg_t result_msg;

    // result pointer
    struct rdma_result_info_t* info;

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
                              result_msg.server_delay, result_msg.result);
                // load a result
                info = &dstip2result[result_msg.dstip];
                if (info->ts_start == 0) {
                    info->ts_start = result_msg.time_ping_send;  // initialize
                }
                info->ts_end = result_msg.time_ping_send;

                if (result_msg.result == PINGWEAVE_RESULT_SUCCESS) {  // success
                    ++info->n_success;
                    info->client_delays.push_back(result_msg.client_delay);
                    info->network_delays.push_back(result_msg.network_delay);
                    info->server_delays.push_back(result_msg.server_delay);
                } else if (result_msg.result ==
                           PINGWEAVE_RESULT_FAILURE) {  // failure
                    ++info->n_failure;
                } else {  // weird
                    ++info->n_weird;
                }
            }

            // Check the interval for report
            auto current_time = std::chrono::steady_clock::now();
            auto elapsed_time =
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    current_time - last_report_time)
                    .count();

            if (elapsed_time >= report_interval_ms) {
                // aggregated results
                std::string agg_result = "";

                for (auto& [dstip, result_info] : dstip2result) {
                    result_stat_t client_stat =
                        calc_result_stats(result_info.client_delays);
                    result_stat_t network_stat =
                        calc_result_stats(result_info.network_delays);
                    result_stat_t server_stat =
                        calc_result_stats(result_info.server_delays);

                    auto result = convert_rdma_result_to_str(
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
    enum spdlog::level::level_enum log_level_client;
    std::shared_ptr<spdlog::logger> client_logger;
    if (get_log_config_from_ini(log_level_client,
                                "logger_cpp_process_rdma_client")) {
        client_logger =
            initialize_logger(client_logname, DIR_LOG_PATH, log_level_client,
                              LOG_FILE_SIZE, LOG_FILE_EXTRA_NUM);
        client_logger->info("RDMA Client is running on pid {}", getpid());
    } else {
        throw std::runtime_error(
            "Failed to get a param 'logger_cpp_process_rdma_client'");
    }

    // Inter-thread queue
    const std::string result_logname = "rdma_" + ipv4;
    enum spdlog::level::level_enum log_level_result;
    std::shared_ptr<spdlog::logger> result_logger;
    if (get_log_config_from_ini(log_level_result,
                                "logger_cpp_process_rdma_result")) {
        result_logger =
            initialize_logger(result_logname, DIR_RESULT_PATH, log_level_result,
                              LOG_FILE_SIZE, LOG_FILE_EXTRA_NUM);
        result_logger->info("RDMA Result is running on pid {}", getpid());
    } else {
        throw std::runtime_error(
            "Failed to get a param 'logger_cpp_process_rdma_result'");
    }

    // Internal message-queue
    RdmaClientQueue client_queue(QUEUE_SIZE);

    // ping table with timeout
    const std::string ping_table_logname = "rdma_table_" + ipv4;
    enum spdlog::level::level_enum log_level_ping_table;
    std::shared_ptr<spdlog::logger> ping_table_logger;
    if (get_log_config_from_ini(log_level_ping_table,
                                "logger_cpp_process_rdma_ping_table")) {
        ping_table_logger = initialize_logger(
            ping_table_logname, DIR_LOG_PATH, log_level_ping_table,
            LOG_FILE_SIZE, LOG_FILE_EXTRA_NUM);
        ping_table_logger->info("RDMA ping_table is running on pid {}",
                                getpid());
    } else {
        throw std::runtime_error(
            "Failed to get a param 'logger_cpp_process_rdma_ping_table'");
    }

    RdmaPinginfoMap ping_table(ping_table_logger, &client_queue,
                               PINGWEAVE_TABLE_EXPIRY_TIME_RDMA_MS);

    // Initialize RDMA contexts
    rdma_context ctx_tx, ctx_rx;
    if (initialize_contexts(ctx_tx, ctx_rx, ipv4, client_logger)) {
        throw std::runtime_error(
            "Client main - Failed to initialize RDMA contexts.");
    }

    // Start the Result thread
    client_logger->info("Starting RDMA result thread (Thread ID: {})...",
                        get_thread_id());
    std::thread result_thread(rdma_client_result_thread, ipv4, &client_queue,
                              result_logger);

    // Start the RX thread
    std::thread rx_thread(rdma_client_rx_thread, &ctx_rx, ipv4, &ping_table,
                          client_logger);

    // Start the TX thread (scheduler)
    std::thread tx_sched_thread(rdma_client_tx_sched_thread, &ctx_tx, ipv4,
                                &ping_table, ctx_rx.gid, ctx_rx.portinfo.lid,
                                ctx_rx.qp->qp_num, client_logger);

    // Start the TX thread (CQE handler)
    std::thread tx_cqe_thread(rdma_client_tx_cqe_thread, &ctx_tx, ipv4,
                              &ping_table, ctx_rx.gid, ctx_rx.portinfo.lid,
                              ctx_rx.qp->qp_num, client_logger);

    // termination
    if (tx_sched_thread.joinable()) {
        tx_sched_thread.join();
    }

    if (tx_cqe_thread.joinable()) {
        tx_cqe_thread.join();
    }

    if (rx_thread.joinable()) {
        rx_thread.join();
    }
    
    if (result_thread.joinable()) {
        result_thread.join();
    }

}

