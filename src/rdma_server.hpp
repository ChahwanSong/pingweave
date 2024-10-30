#pragma once
#include "rdma_common.hpp"

void server_rx_thread(const std::string& ipv4, const std::string& logname,
                      ServerInternalQueue* server_queue,
                      struct pingweave_context* ctx_rx) {
    spdlog::get(logname)->info("Running RX thread (pid: {})...", getpid());

    int ret = 0;
    uint64_t cqe_time = 0;
    struct timespec cqe_ts;
    struct ibv_wc wc = {};

    try {
        // post 1 RECV WR
        if (post_recv(ctx_rx, 1, PINGWEAVE_WRID_RECV) == 0) {
            spdlog::get(logname)->warn("RECV post is failed.");
        }
        // register an event alarm of cq
        if (ibv_req_notify_cq(pingweave_cq(ctx_rx), 0)) {
            spdlog::get(logname)->error("Couldn't register CQE notification");
            throw std::runtime_error("Couldn't register CQE notification");
        }

        // Polling loop
        while (true) {
            spdlog::get(logname)->debug("Wait polling RX RECV CQE...");

            /**
             * IMPORTANT: Event-driven polling via completion channel.
             **/
            struct ibv_cq* ev_cq;
            void* ev_ctx;
            if (ibv_get_cq_event(ctx_rx->channel, &ev_cq, &ev_ctx)) {
                spdlog::get(logname)->error("Failed to get cq_event");
                throw std::runtime_error("Failed to get cq_event");
            }

            // ACK the CQ events
            ibv_ack_cq_events(pingweave_cq(ctx_rx), 1);
            // check the cqe is from a correct CQ
            if (ev_cq != pingweave_cq(ctx_rx)) {
                spdlog::get(logname)->error("CQ event for unknown CQ");
                throw std::runtime_error("CQ event for unknown CQ");
            }

            // re-register an event alarm of cq
            if (ibv_req_notify_cq(pingweave_cq(ctx_rx), 0)) {
                spdlog::get(logname)->error(
                    "Couldn't register CQE notification");
                throw std::runtime_error("Couldn't register CQE notification");
            }
            /*------------------------------------------------------*/

            struct ibv_poll_cq_attr attr = {};
            if (ctx_rx->rnic_hw_ts) {  // RNIC timestamping
                // initialize polling CQ in case of HW timestamp usage
                ret = ibv_start_poll(ctx_rx->cq_s.cq_ex, &attr);
                if (ret) {
                    spdlog::get(logname)->error("ibv_start_poll is failed: {}",
                                                ret);
                    throw std::runtime_error("ibv_start_poll is failed.");
                }
                /** TODO:
                 * ibv_next_poll gets the next item of batch (~16
                 * items). (for further performance optimization)
                 **/

                // if (ibv_next_poll(ctx_rx->cq_s.cq_ex) == ENOENT) {
                //     spdlog::get(logname)->error("CQE event does not exist");
                //     throw std::runtime_error("CQE event does not exist");
                // }
                wc = {0};
                wc.status = ctx_rx->cq_s.cq_ex->status;
                wc.wr_id = ctx_rx->cq_s.cq_ex->wr_id;
                wc.opcode = ibv_wc_read_opcode(ctx_rx->cq_s.cq_ex);
                cqe_time = ibv_wc_read_completion_ts(ctx_rx->cq_s.cq_ex);

                // finish polling CQ
                ibv_end_poll(ctx_rx->cq_s.cq_ex);

            } else {  // app-layer timestamping
                if (ibv_poll_cq(pingweave_cq(ctx_rx), 1, &wc) != 1) {
                    spdlog::get(logname)->error("CQE poll receives nothing");
                    throw std::runtime_error("CQE poll receives nothing");
                }
                if (clock_gettime(CLOCK_MONOTONIC, &cqe_ts) == -1) {
                    spdlog::get(logname)->error(
                        "Failed to run clock_gettime()");
                    throw std::runtime_error("Failed to run clock_gettime()");
                }
                cqe_time = cqe_ts.tv_sec * 1000000000LL + cqe_ts.tv_nsec;
            }

            // post 1 RECV WR
            if (post_recv(ctx_rx, 1, PINGWEAVE_WRID_RECV) == 0) {
                spdlog::get(logname)->warn("RECV post is failed.");
            }

            if (wc.status == IBV_WC_SUCCESS) {
                if (wc.opcode == IBV_WC_RECV) {
                    spdlog::get(logname)->debug("[CQE] RECV (wr_id: {})",
                                                wc.wr_id);

                    // GRH header parsing (for debugging)
                    struct ibv_grh* grh =
                        reinterpret_cast<struct ibv_grh*>(ctx_rx->buf);
                    spdlog::get(logname)->debug("  -> from: {}",
                                                parsed_gid(&grh->sgid));

                    // ping message parsing
                    union ping_msg_t ping_msg;
                    std::memcpy(&ping_msg, ctx_rx->buf + GRH_SIZE,
                                sizeof(ping_msg_t));
                    ping_msg.x.time = cqe_time;

                    // for debugging
                    spdlog::get(logname)->debug(
                        "  -> id : {}, gid: {}, qpn: {}, time: {}",
                        ping_msg.x.pingid, parsed_gid(&ping_msg.x.gid),
                        ping_msg.x.qpn, ping_msg.x.time);

                    if (!server_queue->try_enqueue(ping_msg)) {
                        spdlog::get(logname)->error(
                            "Failed to enqueue ping message");
                        throw std::runtime_error(
                            "Failed to enqueue ping message");
                    }
                } else {
                    spdlog::get(logname)->error(
                        "SEND WC should not occur in Server RX thread");
                    throw std::runtime_error(
                        "SEND WC should not occur in Server RX thread");
                }
            } else {
                spdlog::get(logname)->warn(
                    "RX WR failure - status: {}, opcode: {}",
                    ibv_wc_status_str(wc.status), static_cast<int>(wc.opcode));
                throw std::runtime_error("RX WR failure");
            }
        }
    } catch (const std::exception& e) {
        spdlog::get(logname)->error("RX thread exits unexpectedly.");
        throw;  // Propagate exception
    }
}

void rdma_server(const std::string& ipv4) {
    // logger
    spdlog::drop_all();
    const std::string logname = "rdma_server_" + ipv4;
    auto logger = initialize_custom_logger(logname, LOG_LEVEL_SERVER);

    // internal queue
    ServerInternalQueue server_queue(QUEUE_SIZE);

    // RDMA context
    struct pingweave_context ctx_tx, ctx_rx;
    if (make_ctx(&ctx_tx, ipv4, logname, true, false)) {
        logger->error("Failed to make TX device info: {}", ipv4);
        raise;
    }

    if (make_ctx(&ctx_rx, ipv4, logname, true, true)) {
        logger->error("Failed to make RX device info: {}", ipv4);
        raise;
    }

    // Start RX thread
    std::thread rx_thread(server_rx_thread, ipv4, logname, &server_queue,
                          &ctx_rx);

    /*********************************************************************/
    // main thread loop - handle messages
    logger->info("Running main (TX) thread (pid: {})...", getpid());

    // Create the table (entry timeout = 1 second)
    PingMsgMap pong_table(1);

    // variables
    union ping_msg_t ping_msg;
    union pong_msg_t pong_msg;
    uint64_t cqe_time, var_time;
    struct timespec cqe_ts, var_ts;

    while (true) {
        /**
         * SEND response (PONG) once it receives PING
         * Memorize the ping ID -> {pingid, qpn, gid, time_ping}
         */
        ping_msg = {0};
        if (server_queue.try_dequeue(ping_msg)) {
            logger->debug(
                "Internal queue received a ping_msg - pingid: {}, qpn: {}, "
                "gid: {}, "
                "ping arrival time: {}",
                ping_msg.x.pingid, ping_msg.x.qpn, parsed_gid(&ping_msg.x.gid),
                ping_msg.x.time);

            // (1) memorize
            if (!pong_table.insert(ping_msg.x.pingid, ping_msg)) {
                spdlog::get(logname)->warn(
                    "Failed to insert pingid {} into pong_table.",
                    ping_msg.x.pingid);
            }

            // (2) send the response (what / where)
            pong_msg = {0};
            pong_msg.x.opcode = PINGWEAVE_OPCODE_PONG;
            pong_msg.x.pingid = ping_msg.x.pingid;
            pong_msg.x.server_delay = 0;

            union rdma_addr dst_addr;
            dst_addr.x.gid = ping_msg.x.gid;
            dst_addr.x.qpn = ping_msg.x.qpn;

            spdlog::get(logname)->debug(
                "SEND post with PONG message of pingid: {} to qpn: {}, gid: {}",
                pong_msg.x.pingid, dst_addr.x.qpn, parsed_gid(&dst_addr.x.gid));
            if (post_send(&ctx_tx, dst_addr, pong_msg.raw, sizeof(pong_msg_t),
                          pong_msg.x.pingid)) {
                if (check_log(ctx_tx.log_msg)) {
                    spdlog::get(logname)->error(ctx_tx.log_msg);
                }
                throw std::runtime_error("SEND PONG post is failed");
            }
        }

        /**
         * CQE capture
         * IMPORTANT: we use non-blocking polling here.
         *
         * Case 1: CQE of Pong RECV -> ACK SEND
         * Get CQE time of PONG SEND, and calculate delay
         * Send ACK with the time and remove entry from pong_table
         * wr_id is the ping ID
         *
         * Case 2: CQE of ACK -> ignore
         * wr_id is PINGWEAVE_WRID_SEND
         **/

        struct ibv_poll_cq_attr attr = {};
        struct ibv_wc wc = {};
        int end_flag = false;
        int ret;
        if (ctx_tx.rnic_hw_ts) {  // extension
            // initialize polling CQ in case of HW timestamp usage
            ret = ibv_start_poll(ctx_tx.cq_s.cq_ex, &attr);

            while (!ret) {  // ret == 0 if success
                end_flag = true;

                /* do something */
                wc = {0};
                wc.status = ctx_tx.cq_s.cq_ex->status;  // status
                wc.wr_id = ctx_tx.cq_s.cq_ex->wr_id;    // pingid
                wc.opcode = ibv_wc_read_opcode(ctx_tx.cq_s.cq_ex);
                cqe_time = ibv_wc_read_completion_ts(ctx_tx.cq_s.cq_ex);

                if (wc.status == IBV_WC_SUCCESS) {
                    if (wc.opcode == IBV_WC_SEND) {
                        spdlog::get(logname)->debug("[CQE] SEND (wr_id: {})",
                                                    wc.wr_id);
                        if (wc.wr_id == PINGWEAVE_WRID_SEND) {  // ACK - ignore
                            spdlog::get(logname)->debug(
                                "-> CQE of ACK, so ignore this");
                        } else {
                            spdlog::get(logname)->debug("-> PONG's pingID: {}",
                                                        wc.wr_id);
                            ping_msg = {0};
                            if (pong_table.get(wc.wr_id, ping_msg)) {
                                // generate ACK message
                                pong_msg = {0};
                                pong_msg.x.opcode = PINGWEAVE_OPCODE_ACK;
                                pong_msg.x.pingid = wc.wr_id;
                                pong_msg.x.server_delay =
                                    calc_time_delta_with_bitwrap(
                                        ping_msg.x.time, cqe_time,
                                        ctx_tx.completion_timestamp_mask);
                                spdlog::get(logname)->debug(
                                    "SEND post with ACK message pingid: {} to "
                                    "qpn: "
                                    "{}, gid: {}, delay: {}",
                                    pong_msg.x.pingid, ping_msg.x.qpn,
                                    parsed_gid(&ping_msg.x.gid),
                                    pong_msg.x.server_delay);

                                union rdma_addr dst_addr;
                                dst_addr.x.gid = ping_msg.x.gid;
                                dst_addr.x.qpn = ping_msg.x.qpn;

                                if (post_send(&ctx_tx, dst_addr, pong_msg.raw,
                                              sizeof(pong_msg_t),
                                              PINGWEAVE_WRID_SEND)) {
                                    if (check_log(ctx_tx.log_msg)) {
                                        spdlog::get(logname)->error(
                                            ctx_tx.log_msg);
                                    }
                                    throw std::runtime_error(
                                        "SEND ACK post is failed");
                                }
                            } else {
                                spdlog::get(logname)->warn(
                                    "pingid {} entry does not exist (expired?)",
                                    wc.wr_id);
                            }
                            // erase
                            if (!pong_table.remove(wc.wr_id)) {
                                spdlog::get(logname)->warn(
                                    "Nothing to remove the id {} from timedMap",
                                    wc.wr_id);
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
                    throw std::runtime_error("RX WR failure");
                }

                // next round
                ret = ibv_next_poll(ctx_tx.cq_s.cq_ex);
            }
            if (end_flag) {
                ibv_end_poll(ctx_tx.cq_s.cq_ex);
            }
        } else {  // original
            ret = ibv_poll_cq(pingweave_cq(&ctx_tx), 1, &wc);
            while (ret) {  // ret == 1 if success
                // get current time
                if (clock_gettime(CLOCK_MONOTONIC, &cqe_ts) == -1) {
                    spdlog::get(logname)->error(
                        "Failed to run clock_gettime()");
                    throw std::runtime_error("clock_gettime is failed");
                }
                cqe_time = cqe_ts.tv_sec * 1000000000LL + cqe_ts.tv_nsec;

                if (wc.status == IBV_WC_SUCCESS) {
                    if (wc.opcode == IBV_WC_SEND) {
                        spdlog::get(logname)->debug("[CQE] SEND (wr_id: {})",
                                                    wc.wr_id);
                        if (wc.wr_id == PINGWEAVE_WRID_SEND) {  // ACK - ignore
                            spdlog::get(logname)->debug(
                                "CQE of ACK, so ignore this");
                        } else {
                            spdlog::get(logname)->debug("-> PONG's pingID: {}",
                                                        wc.wr_id);
                            ping_msg = {0};
                            if (pong_table.get(wc.wr_id, ping_msg)) {
                                // generate ACK message
                                pong_msg = {0};
                                pong_msg.x.opcode = PINGWEAVE_OPCODE_ACK;
                                pong_msg.x.pingid = wc.wr_id;
                                pong_msg.x.server_delay =
                                    calc_time_delta_with_bitwrap(
                                        ping_msg.x.time, cqe_time,
                                        ctx_tx.completion_timestamp_mask);
                                spdlog::get(logname)->debug(
                                    "SEND post with ACK message pingid: {} to "
                                    "qpn: "
                                    "{}, gid: {}, delay: {}",
                                    pong_msg.x.pingid, ping_msg.x.qpn,
                                    parsed_gid(&ping_msg.x.gid),
                                    pong_msg.x.server_delay);

                                union rdma_addr dst_addr;
                                dst_addr.x.gid = ping_msg.x.gid;
                                dst_addr.x.qpn = ping_msg.x.qpn;

                                if (post_send(&ctx_tx, dst_addr, pong_msg.raw,
                                              sizeof(pong_msg_t),
                                              PINGWEAVE_WRID_SEND)) {
                                    if (check_log(ctx_tx.log_msg)) {
                                        spdlog::get(logname)->error(
                                            ctx_tx.log_msg);
                                    }
                                    throw std::runtime_error(
                                        "SEND ACK post is failed");
                                }
                            } else {
                                spdlog::get(logname)->warn(
                                    "pingid {} entry does not exist (expired?)",
                                    wc.wr_id);
                            }
                            // erase
                            if (!pong_table.remove(wc.wr_id)) {
                                spdlog::get(logname)->warn(
                                    "Nothing to remove the id {} from "
                                    "pong_table",
                                    wc.wr_id);
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
                    throw std::runtime_error("RX WR failure");
                }

                // next round
                ret = ibv_poll_cq(pingweave_cq(&ctx_tx), 1, &wc);
            }
        }

        std::this_thread::sleep_for(std::chrono::microseconds(10));
    }

    // thread handling
    if (rx_thread.joinable()) {
        rx_thread.join();
    }
}