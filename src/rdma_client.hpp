#pragma once

#include "ping_info_map.hpp"
#include "ping_msg_map.hpp"
#include "rdma_common.hpp"
#include "rdma_scheduler.hpp"

// Function to handle received messages
void handle_received_message(pingweave_context* ctx_rx,
                             const union pong_msg_t& pong_msg,
                             PingInfoMap* ping_table, uint64_t recv_time,
                             uint64_t cqe_time,
                             std::shared_ptr<spdlog::logger> logger) {
    if (pong_msg.x.opcode == PINGWEAVE_OPCODE_PONG) {
        // Handle PONG message
        logger->debug("[CQE] Recv PONG ({}): recv_time {}, cqe_time:{}",
                      pong_msg.x.pingid, recv_time, cqe_time);
        if (!ping_table->update_pong_info(pong_msg.x.pingid, recv_time,
                                          UINT64_MAX, cqe_time,
                                          ctx_rx->completion_timestamp_mask)) {
            logger->warn("PONG ({}): No entry in ping_table.",
                         pong_msg.x.pingid);
        }

    } else if (pong_msg.x.opcode == PINGWEAVE_OPCODE_ACK) {
        // Handle ACK message
        logger->debug("[CQE] Recv PONG_ACK ({}): server_delay {}",
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

// Function to initialize RDMA contexts
bool initialize_contexts(pingweave_context& ctx_tx, pingweave_context& ctx_rx,
                         const std::string& ipv4,
                         std::shared_ptr<spdlog::logger> logger) {
    if (make_ctx(&ctx_tx, ipv4, logger, false)) {
        logger->error("Failed to create TX context for IP: {}", ipv4);
        return true;
    }
    if (make_ctx(&ctx_rx, ipv4, logger, true)) {
        logger->error("Failed to create RX context for IP: {}", ipv4);
        return true;
    }
    return false;
}

// Function to process TX CQEs
void process_tx_cqe(pingweave_context* ctx_tx, PingInfoMap* ping_table,
                    std::shared_ptr<spdlog::logger> logger) {
    uint64_t cqe_time = 0;
    struct ibv_wc wc = {};
    int ret = 0;

    /**
     * IMPORTANT: Use non-blocking polling.
     * Otherwise, scheduling the next message to send will be blocked.
     */

    if (ctx_tx->rnic_hw_ts) {
        // Use extended CQ polling for hardware timestamping
        struct ibv_poll_cq_attr attr = {};
        ret = ibv_start_poll(ctx_tx->cq_s.cq_ex, &attr);
        bool has_events = false;

        while (!ret) {
            has_events = true;

            // Process the current CQE
            wc.status = ctx_tx->cq_s.cq_ex->status;
            wc.wr_id = ctx_tx->cq_s.cq_ex->wr_id;
            wc.opcode = ibv_wc_read_opcode(ctx_tx->cq_s.cq_ex);
            cqe_time = ibv_wc_read_completion_ts(ctx_tx->cq_s.cq_ex);

            if (wc.status == IBV_WC_SUCCESS && wc.opcode == IBV_WC_SEND) {
                logger->debug("[CQE] Send completed (ping ID: {}), time: {}",
                              wc.wr_id, cqe_time);
                if (!ping_table->update_ping_cqe_time(wc.wr_id, cqe_time)) {
                    logger->warn(
                        "Failed to update send completion time for ping ID {}.",
                        wc.wr_id);
                }
            } else {
                logger->error("TX WR failure - status: {}, opcode: {}",
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
        ret = ibv_poll_cq(pingweave_cq(ctx_tx), 1, &wc);

        if (!ret) {
            // nothing to poll. add a small jittering.
            std::this_thread::sleep_for(std::chrono::microseconds(10));
            return;
        }

        while (ret) {
            cqe_time = get_current_timestamp_steady();

            if (wc.status == IBV_WC_SUCCESS && wc.opcode == IBV_WC_SEND) {
                logger->debug("[CQE] Send completed (ping ID: {}), time: {}",
                              wc.wr_id, cqe_time);
                if (!ping_table->update_ping_cqe_time(wc.wr_id, cqe_time)) {
                    logger->warn(
                        "Failed to update send completion time for ping ID {}.",
                        wc.wr_id);
                }
            } else {
                logger->error("TX WR failure - status: {}, opcode: {}",
                              ibv_wc_status_str(wc.status),
                              static_cast<int>(wc.opcode));
                throw std::runtime_error("TX WR failure");
            }

            // poll next event
            ret = ibv_poll_cq(pingweave_cq(ctx_tx), 1, &wc);
        }
    }
}

// Function to process RX CQEs
void process_rx_cqe(pingweave_context* ctx_rx, PingInfoMap* ping_table,
                    std::shared_ptr<spdlog::logger> logger) {
    uint64_t cqe_time = 0, recv_time = 0;
    struct ibv_wc wc = {};
    int ret = 0;
    union pong_msg_t pong_msg = {};
    try {
        if (ctx_rx->rnic_hw_ts) {
            // Use extended CQ polling for hardware timestamping
            struct ibv_poll_cq_attr attr = {};
            ret = ibv_start_poll(ctx_rx->cq_s.cq_ex, &attr);
            bool has_events = false;

            while (!ret) {
                has_events = true;

                // Parse the received message
                std::memcpy(&pong_msg, ctx_rx->buf + GRH_SIZE,
                            sizeof(pong_msg_t));

                // get recv time
                recv_time = get_current_timestamp_steady();

                // Process the current CQE
                wc.status = ctx_rx->cq_s.cq_ex->status;
                wc.wr_id = ctx_rx->cq_s.cq_ex->wr_id;
                wc.opcode = ibv_wc_read_opcode(ctx_rx->cq_s.cq_ex);
                cqe_time = ibv_wc_read_completion_ts(ctx_rx->cq_s.cq_ex);

                if (wc.status == IBV_WC_SUCCESS && wc.opcode == IBV_WC_RECV) {
                    // Handle the received message (PONG or ACK)
                    handle_received_message(ctx_rx, pong_msg, ping_table,
                                            recv_time, cqe_time, logger);

                    // Post the next RECV WR
                    if (post_recv(ctx_rx, 1, PINGWEAVE_WRID_RECV) == 0) {
                        logger->error("Failed to post the next RECV WR.");
                        throw std::runtime_error("RX RECV post failed");
                    }
                } else {
                    logger->error("RX WR failure - status: {}, opcode: {}",
                                  ibv_wc_status_str(wc.status),
                                  static_cast<int>(wc.opcode));
                    throw std::runtime_error("RX WR failure");
                }

                ret = ibv_next_poll(ctx_rx->cq_s.cq_ex);
            }

            if (has_events) {
                // End the polling session
                ibv_end_poll(ctx_rx->cq_s.cq_ex);
            } else {
                // nothing to poll
                return;
            }
        } else {
            ret = ibv_poll_cq(pingweave_cq(ctx_rx), 1, &wc);

            if (!ret) {
                // nothing to poll
                return;
            }

            while (ret) {
                // Parse the received message
                std::memcpy(&pong_msg, ctx_rx->buf + GRH_SIZE,
                            sizeof(pong_msg_t));

                // Get current time
                cqe_time = get_current_timestamp_steady();
                recv_time = cqe_time;

                if (wc.status == IBV_WC_SUCCESS && wc.opcode == IBV_WC_RECV) {
                    // Handle the received message (PONG or ACK)
                    handle_received_message(ctx_rx, pong_msg, ping_table,
                                            recv_time, cqe_time, logger);

                    // Post the next RECV WR
                    if (post_recv(ctx_rx, 1, PINGWEAVE_WRID_RECV) == 0) {
                        logger->error("Failed to post the next RECV WR.");
                        throw std::runtime_error("RX RECV post failed");
                    }
                } else {
                    logger->error("RX WR failure - status: {}, opcode: {}",
                                  ibv_wc_status_str(wc.status),
                                  static_cast<int>(wc.opcode));
                    throw std::runtime_error("RX WR failure");
                }

                // poll next event
                ret = ibv_poll_cq(pingweave_cq(ctx_rx), 1, &wc);
            }
        }
    } catch (const std::exception& e) {
        logger->error("RX thread exits unexpectedly: {}", e.what());
        throw;  // Propagate exception
    }
}

void client_tx_thread(const std::string& ipv4, const std::string& logname,
                      PingInfoMap* ping_table, struct pingweave_context* ctx_tx,
                      const union ibv_gid& rx_gid, const uint32_t& rx_lid,
                      const uint32_t& rx_qpn) {
    auto logger = spdlog::get(logname);
    logger->info("Starting TX thread after 1 second (Thead ID: {})...",
                 get_thread_id());
    std::this_thread::sleep_for(std::chrono::seconds(1));

    uint64_t ping_id = PING_ID_INIT;  // start with a large number (> 100)
    MsgScheduler scheduler(ipv4, logname);

    try {
        while (true) {
            // Retrieve the next destination for sending
            std::tuple<std::string, std::string, uint32_t, uint32_t> dst_info;
            if (scheduler.next(dst_info)) {
                const auto& [dst_ip, dst_gid_str, dst_lid, dst_qpn] = dst_info;

                // Set the destination address
                union rdma_addr dst_addr = {};
                dst_addr.x.qpn = dst_qpn;
                wire_gid_to_gid(dst_gid_str.c_str(), &dst_addr.x.gid);
                dst_addr.x.lid = dst_lid;

                // Create the PING message
                union ping_msg_t msg = {};
                msg.x.pingid = ping_id++;
                msg.x.qpn = rx_qpn;
                msg.x.gid = rx_gid;
                msg.x.lid = rx_lid;

                if (msg.x.pingid < PING_ID_INIT) {
                    logger->error("Ping ID must be at least {}. Current: {}",
                                  PING_ID_INIT, msg.x.pingid);
                    throw std::runtime_error("Invalid ping ID.");
                }

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
                    "{}), "
                    "time: {}",
                    msg.x.pingid, msg.x.qpn, parsed_gid(&msg.x.gid), msg.x.lid,
                    send_time_steady);
                if (post_send(ctx_tx, dst_addr, msg.raw, sizeof(ping_msg_t),
                              msg.x.pingid)) {
                    throw std::runtime_error("Failed to send PING message.");
                }
            }

            /** NOTE: Client's TX loop must be non-blocking. */
            // Process TX CQEs
            process_tx_cqe(ctx_tx, ping_table, logger);
        }
    } catch (const std::exception& e) {
        logger->error("Exception in TX thread: {}", e.what());
        throw;  // Propagate exception
    }
}

void client_rx_thread(const std::string& ipv4, const std::string& logname,
                      PingInfoMap* ping_table,
                      struct pingweave_context* ctx_rx) {
    auto logger = spdlog::get(logname);
    logger->info("Running RX thread (Thread ID: {})...", get_thread_id());

    // Post RECV WRs and register CQ event notifications
    if (post_recv(ctx_rx, RX_DEPTH, PINGWEAVE_WRID_RECV) < RX_DEPTH) {
        logger->warn("Failed to post RECV WRs.");
    }

    // Register for CQ event notifications
    if (ibv_req_notify_cq(pingweave_cq(ctx_rx), 0)) {
        logger->error("Couldn't register CQE notification");
        throw std::runtime_error("Couldn't register CQE notification");
    }

    // Start the receive loop
    try {
        while (true) {
            /** IMPORTANT: Use event-driven polling to reduce CPU overhead. */
            if (!wait_for_cq_event(ctx_rx, logger)) {  // Wait for CQ event
                throw std::runtime_error("Failed during CQ event waiting");
            }

            // Process RX CQEs
            process_rx_cqe(ctx_rx, ping_table, logger);
        }
    } catch (const std::exception& e) {
        logger->error("Exception in RX thread: {}", e.what());
    }
}

void client_result_thread(const std::string& ipv4, const std::string& logname,
                          const std::string& client_logname,
                          ClientInternalQueue* client_queue) {
    auto logger = spdlog::get(logname);
    spdlog::get(client_logname)
        ->info("Starting result thread (Thread ID: {})...", get_thread_id());

    // dstip -> result history
    std::unordered_map<uint32_t, struct result_info_t> dstip2result;

    // msg from RX Thread
    struct result_msg_t result_msg;

    // result pointer
    struct result_info_t* info;

    // timer for report
    auto last_report_time = std::chrono::steady_clock::now();

    /** OUTPUT: (#success, #failure, mean, max, p50, p95, p99) */
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

            // report periodically for every 1 minute
            auto current_time = std::chrono::steady_clock::now();
            auto elapsed_time =
                std::chrono::duration_cast<std::chrono::seconds>(
                    current_time - last_report_time)
                    .count();

            if (elapsed_time >= REPORT_INTERVAL_SEC) {
                for (auto& [dstip, result_info] : dstip2result) {
                    result_stat_t client_stat =
                        calculateStatistics(result_info.client_delays);
                    result_stat_t network_stat =
                        calculateStatistics(result_info.network_delays);
                    result_stat_t server_stat =
                        calculateStatistics(result_info.server_delays);

                    // logging
                    logger->info(
                        "{},{},{},{},{},Client:{},{},{},{},{},Network:{},{},{},"
                        "{},{},Server:{},{},{},{},{}",
                        uint2ip(dstip),
                        timestamp_ns_to_string(result_info.ts_start),
                        timestamp_ns_to_string(result_info.ts_end),
                        result_info.n_success, result_info.n_failure,
                        client_stat.mean, client_stat.max,
                        client_stat.percentile_50, client_stat.percentile_95,
                        client_stat.percentile_99, network_stat.mean,
                        network_stat.max, network_stat.percentile_50,
                        network_stat.percentile_95, network_stat.percentile_99,
                        server_stat.mean, server_stat.max,
                        server_stat.percentile_50, server_stat.percentile_95,
                        server_stat.percentile_99);
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
    std::shared_ptr<spdlog::logger> client_logger = initialize_custom_logger(
        client_logname, LOG_LEVEL_CLIENT, LOG_FILE_SIZE, LOG_FILE_EXTRA_NUM);
    client_logger->info("RDMA Client is running on pid {}", getpid());

    // Inter-thread queue
    const std::string result_logname = "rdma_" + ipv4;
    std::shared_ptr<spdlog::logger> result_logger = initialize_result_logger(
        result_logname, LOG_LEVEL_RESULT, LOG_FILE_SIZE, LOG_FILE_EXTRA_NUM);
    ClientInternalQueue client_queue(QUEUE_SIZE);

    // ping table (timeout =  1 second)
    const std::string ping_table_logname = "rdma_table_" + ipv4;
    std::shared_ptr<spdlog::logger> ping_table_logger =
        initialize_custom_logger(ping_table_logname, LOG_LEVEL_PING_TABLE,
                                 LOG_FILE_SIZE, LOG_FILE_EXTRA_NUM);
    PingInfoMap ping_table(ping_table_logger, &client_queue, 1);

    // Initialize RDMA contexts
    pingweave_context ctx_tx = {};
    pingweave_context ctx_rx = {};
    if (initialize_contexts(ctx_tx, ctx_rx, ipv4, client_logger)) {
        throw std::runtime_error("Failed to initialize RDMA contexts.");
    }

    // Start the Result thread
    std::thread result_thread(client_result_thread, ipv4, result_logname,
                              client_logname, &client_queue);

    // Start the RX thread
    std::thread rx_thread(client_rx_thread, ipv4, client_logname, &ping_table,
                          &ctx_rx);

    // Start the TX thread
    std::thread tx_thread(client_tx_thread, ipv4, client_logname, &ping_table,
                          &ctx_tx, ctx_rx.gid, ctx_rx.portinfo.lid,
                          ctx_rx.qp->qp_num);

    // termination
    if (tx_thread.joinable()) {
        tx_thread.join();
    }

    if (result_thread.joinable()) {
        result_thread.join();
    }
}