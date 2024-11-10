#pragma once

#include "ping_info_map.hpp"
#include "ping_msg_map.hpp"
#include "rdma_common.hpp"
#include "rdma_scheduler.hpp"

/**
 * NOTE: Event-driven polling makes a buffer over-written (i.e., msg loss).
 * This is because of RDMA UD - unreliable protocol.
 */
#ifndef PINGWEAVE_CLIENT_RX_EVENT_DRIVEN
#define PINGWEAVE_CLIENT_RX_EVENT_DRIVEN (0)  // 1: event-driven polling
#endif

// Function to handle received messages
void handle_received_message(pingweave_context* ctx_rx,
                             const union pong_msg_t& pong_msg,
                             PingInfoMap* ping_table, uint64_t recv_time,
                             uint64_t cqe_time, const std::string& logname) {
    auto logger = spdlog::get(logname);

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
                         const std::string& ipv4, const std::string& logname) {
    auto logger = spdlog::get(logname);
    if (make_ctx(&ctx_tx, ipv4, logname, false)) {
        logger->error("Failed to create TX context for IP: {}", ipv4);
        return true;
    }
    if (make_ctx(&ctx_rx, ipv4, logname, true)) {
        logger->error("Failed to create RX context for IP: {}", ipv4);
        return true;
    }
    return false;
}

// Function to process TX CQEs
void process_tx_cqe(pingweave_context* ctx_tx, PingInfoMap* ping_table,
                    const std::string& logname) {
    auto logger = spdlog::get(logname);

    struct ibv_wc wc = {};
    uint64_t cqe_time = 0;
    bool has_cqe = false;
    int ret = 0;

    if (ctx_tx->rnic_hw_ts) {
        // Use extended CQ polling for hardware timestamping
        struct ibv_poll_cq_attr attr = {};

        if (ibv_start_poll(ctx_tx->cq_s.cq_ex, &attr) == 0) {  // success
            has_cqe = true;

            // Process the current CQE
            wc.status = ctx_tx->cq_s.cq_ex->status;
            wc.wr_id = ctx_tx->cq_s.cq_ex->wr_id;
            wc.opcode = ibv_wc_read_opcode(ctx_tx->cq_s.cq_ex);
            cqe_time = ibv_wc_read_completion_ts(ctx_tx->cq_s.cq_ex);

            // End the polling session
            ibv_end_poll(ctx_tx->cq_s.cq_ex);
        }
    } else {
        // Standard CQ polling
        if (ibv_poll_cq(pingweave_cq(ctx_tx), 1, &wc) == 1) {  // success
            has_cqe = true;

            // Get current time
            cqe_time = get_current_timestamp_steady();
        }
    }

    if (has_cqe) {
        if (wc.status == IBV_WC_SUCCESS && wc.opcode == IBV_WC_SEND) {
            logger->debug("[CQE] Send completed (ping ID: {}), time: {}",
                          wc.wr_id, cqe_time);
            if (!ping_table->update_ping_cqe_time(wc.wr_id, cqe_time)) {
                logger->error(
                    "Failed to update send completion time for ping ID {}.",
                    wc.wr_id);
            }
        } else {
            logger->warn("TX WR failure - status: {}, opcode: {}",
                         ibv_wc_status_str(wc.status),
                         static_cast<int>(wc.opcode));
        }
    } else {
        std::this_thread::sleep_for(std::chrono::microseconds(10));
    }
}

// Function to process RX CQEs
void process_rx_cqe(pingweave_context* ctx_rx, PingInfoMap* ping_table,
                    const std::string& logname) {
    auto logger = spdlog::get(logname);

    struct ibv_wc wc = {};
    uint64_t cqe_time = 0, recv_time = 0;
    bool has_cqe = false;
    int ret = 0;
    union pong_msg_t pong_msg = {};

    if (ctx_rx->rnic_hw_ts) {
        // Use extended CQ polling for hardware timestamping
        struct ibv_poll_cq_attr attr = {};

        if (ibv_start_poll(ctx_rx->cq_s.cq_ex, &attr) == 0) {  // success
            has_cqe = true;

            // Parse the received message
            std::memcpy(&pong_msg, ctx_rx->buf + GRH_SIZE, sizeof(pong_msg_t));

            // get recv time
            recv_time = get_current_timestamp_steady();

            // Process the current CQE
            wc.status = ctx_rx->cq_s.cq_ex->status;
            wc.wr_id = ctx_rx->cq_s.cq_ex->wr_id;
            wc.opcode = ibv_wc_read_opcode(ctx_rx->cq_s.cq_ex);
            cqe_time = ibv_wc_read_completion_ts(ctx_rx->cq_s.cq_ex);

            // End the polling session
            ibv_end_poll(ctx_rx->cq_s.cq_ex);
        }
    } else {
        // Standard CQ polling
        if (ibv_poll_cq(pingweave_cq(ctx_rx), 1, &wc) > 0) {  // success
            has_cqe = true;

            // Parse the received message
            std::memcpy(&pong_msg, ctx_rx->buf + GRH_SIZE, sizeof(pong_msg_t));

            // Get current time
            cqe_time = get_current_timestamp_steady();
            recv_time = cqe_time;
        }
    }

    if (has_cqe) {
        if (wc.status == IBV_WC_SUCCESS && wc.opcode == IBV_WC_RECV) {
            // Handle the received message (PONG or ACK)
            handle_received_message(ctx_rx, pong_msg, ping_table, recv_time,
                                    cqe_time, logname);

            // Post the next RECV WR
            if (post_recv(ctx_rx, 1, PINGWEAVE_WRID_RECV) == 0) {
                logger->warn("Failed to post the next RECV WR.");
            }
        } else {
            logger->warn("RX WR failure - status: {}, opcode: {}",
                         ibv_wc_status_str(wc.status),
                         static_cast<int>(wc.opcode));
        }
    } else {
#if (PINGWEAVE_CLIENT_RX_EVENT_DRIVEN != 1)
        // No CQE to process
        std::this_thread::sleep_for(std::chrono::microseconds(10));
#endif
    }
}

void client_tx_thread(const std::string& ipv4, const std::string& logname,
                      PingInfoMap* ping_table, struct pingweave_context* ctx_tx,
                      const union ibv_gid& rx_gid, const uint32_t& rx_qpn) {
    auto logger = spdlog::get(logname);
    logger->info("Starting TX thread after 1 second (PID: {})...", getpid());
    std::this_thread::sleep_for(std::chrono::seconds(1));

    uint64_t ping_id = PING_ID_INIT;
    MsgScheduler scheduler(ipv4, logname);

    try {
        while (true) {
            // Retrieve the next destination for sending
            std::tuple<std::string, uint32_t, std::string> dst_info;
            if (scheduler.next(dst_info)) {
                const auto& [dst_ip, dst_qpn, dst_gid_str] = dst_info;

                // Set the destination address
                union rdma_addr dst_addr = {};
                dst_addr.x.qpn = dst_qpn;
                wire_gid_to_gid(dst_gid_str.c_str(), &dst_addr.x.gid);

                // Create the PING message
                union ping_msg_t msg = {};
                msg.x.pingid = ping_id++;
                msg.x.qpn = rx_qpn;
                msg.x.gid = rx_gid;

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
                        {msg.x.pingid, msg.x.qpn, msg.x.gid, dst_ip,
                         send_time_system, send_time_steady, 0, 0,
                         PINGWEAVE_MASK_INIT})) {
                    logger->warn("Failed to insert ping ID {} into ping_table.",
                                 msg.x.pingid);
                }

                // Send the PING message
                logger->debug(
                    "Sending PING message (ping ID: {}, QPN: {}, GID: {}), "
                    "time: {}",
                    msg.x.pingid, msg.x.qpn, parsed_gid(&msg.x.gid),
                    send_time_steady);
                if (post_send(ctx_tx, dst_addr, msg.raw, sizeof(ping_msg_t),
                              msg.x.pingid)) {
                    throw std::runtime_error("Failed to send PING message.");
                }
            }

            // Process TX CQEs
            process_tx_cqe(ctx_tx, ping_table, logname);
        }
    } catch (const std::exception& e) {
        logger->error("Exception in TX thread: {}", e.what());
    }
}

void client_result_thread(std::shared_ptr<spdlog::logger> logger,
                          std::string ipv4, ClientInternalQueue* client_queue) {
    logger->info("Starting result thread (PID: {})...", getpid());

    struct result_info_t result_info;
    try {
        while (true) {
            // fully-blocking
            client_queue->wait_dequeue(result_info);
            logger->info("{}, {}, {}, {}, {}, {}, {}",
                         timestamp_ns_to_string(result_info.time_ping_send),
                         uint2ip(result_info.dstip), result_info.pingid,
                         result_info.success, result_info.client_delay,
                         result_info.network_delay, result_info.server_delay);
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
    client_logger->info("Running main (RX) thread (PID: {})...", getpid());

    // Inter-thread queue
    const std::string result_logname = "rdma_" + ipv4;
    std::shared_ptr<spdlog::logger> result_logger = initialize_result_logger(
        result_logname, LOG_LEVEL_RESULT, LOG_FILE_SIZE, LOG_FILE_EXTRA_NUM);
    ClientInternalQueue client_queue(QUEUE_SIZE);
    std::thread result_thread(client_result_thread, result_logger, ipv4,
                              &client_queue);

    // ping table (timeout =  1 second)
    const std::string ping_table_logname = "rdma_table_" + ipv4;
    std::shared_ptr<spdlog::logger> ping_table_logger =
        initialize_custom_logger(ping_table_logname, LOG_LEVEL_PING_TABLE,
                                 LOG_FILE_SIZE, LOG_FILE_EXTRA_NUM);
    PingInfoMap ping_table(ping_table_logger, &client_queue, 1);

    // Initialize RDMA contexts
    pingweave_context ctx_tx = {};
    pingweave_context ctx_rx = {};
    if (initialize_contexts(ctx_tx, ctx_rx, ipv4, client_logname)) {
        throw std::runtime_error("Failed to initialize RDMA contexts.");
    }

    // Start the TX thread
    std::thread tx_thread(client_tx_thread, ipv4, client_logname, &ping_table,
                          &ctx_tx, ctx_rx.gid, ctx_rx.qp->qp_num);

    // Post RECV WRs and register CQ event notifications
    if (post_recv(&ctx_rx, RX_DEPTH, PINGWEAVE_WRID_RECV) < RX_DEPTH) {
        client_logger->warn("Failed to post RECV WRs.");
    }

#if (PINGWEAVE_CLIENT_RX_EVENT_DRIVEN == 1)
    if (ibv_req_notify_cq(pingweave_cq(&ctx_rx), 0)) {
        throw std::runtime_error("Failed to register CQ event notifications.");
    }
#endif

    // Start the receive loop
    try {
        while (true) {
#if (PINGWEAVE_CLIENT_RX_EVENT_DRIVEN == 1)
            /**
             * IMPORTANT: Event-driven polling via completion channel.
             **/
            spdlog::get(client_logname)->debug("Wait polling RX RECV CQE...");
            struct ibv_cq* ev_cq;
            void* ev_ctx;
            if (ibv_get_cq_event(ctx_rx.channel, &ev_cq, &ev_ctx)) {
                spdlog::get(client_logname)->error("Failed to get cq_event");
                throw std::runtime_error("Failed to get cq_event");
            }

            // ACK the CQ events
            ibv_ack_cq_events(pingweave_cq(&ctx_rx), 1);
            // check the cqe is from a correct CQ
            if (ev_cq != pingweave_cq(&ctx_rx)) {
                spdlog::get(client_logname)->error("CQ event for unknown CQ");
                throw std::runtime_error("CQ event for unknown CQ");
            }

            // re-register an event alarm of cq
            if (ibv_req_notify_cq(pingweave_cq(&ctx_rx), 0)) {
                spdlog::get(client_logname)
                    ->error("Couldn't register CQE notification");
                throw std::runtime_error("Couldn't register CQE notification");
            }
#endif
            // Process RX CQEs
            process_rx_cqe(&ctx_rx, &ping_table, client_logname);
        }
    } catch (const std::exception& e) {
        client_logger->error("Exception in RX thread: {}", e.what());
    }

    if (tx_thread.joinable()) {
        tx_thread.join();
    }
}