#pragma once
// #include "producer_queue.hpp"
#include "rdma_common.hpp"
#include "rdma_scheduler.hpp"

void client_tx_thread(const std::string& ipv4, const std::string& logname,
                      PingInfoMap* ping_table, struct pingweave_context* ctx_tx,
                      const union ibv_gid& rx_gid, const uint32_t& rx_qpn) {
    spdlog::get(logname)->info("Run TX thread after 1 second (pid: {})...",
                               getpid());
    std::this_thread::sleep_for(std::chrono::seconds(1));

    // initialize pingid which monotonically increases;
    // Note that it should be higher than 2 (PINGWEAVE_WRID_SEND)
    // to distinguish between PONG and ACK at server
    uint64_t it_pingid = PING_ID_INIT;

    // variables
    int ret = 0;
    uint64_t cqe_time, var_time;
    struct timespec cqe_ts, var_ts;

    /**
     * TODO: Schedule SEND PING with multiple destinations
     * pinglist (ipv4) -> get the corresponding (ip, qpn, gid)
     */
    MsgScheduler scheduler(ipv4, logname);

    try {
        // Variables for msg scheduling
        std::tuple<std::string, uint32_t, std::string> dst_next;
        union rdma_addr dst_addr;
        union ping_msg_t msg;
        msg.x.qpn = rx_qpn;
        msg.x.gid = rx_gid;
        msg.x.time = 0;
        msg.x.pingid = 0;

        // SEND CQE message loop
        while (true) {
            /**
             * SEND: Scheduler pops the next message to send.
             */
            if (scheduler.next(dst_next)) {  // if success
                std::string dst_ip = std::get<0>(dst_next);
                uint32_t dst_qpn = std::get<1>(dst_next);
                std::string dst_gid = std::get<2>(dst_next);

                // for debugging
                spdlog::get(logname)->debug("Target IP: {}, GID: {}, QPN: {}",
                                            dst_ip, parsed_gid(&dst_addr.x.gid),
                                            dst_addr.x.qpn);
                /*----------------------------------------------*/

                // target address (i.e., destination)
                dst_addr = {0};
                dst_addr.x.qpn = dst_qpn;                           // qpn
                wire_gid_to_gid(dst_gid.c_str(), &dst_addr.x.gid);  // gid

                // message to send
                msg.x.pingid = it_pingid++;

                // sanity check: wr_id must be large enough
                if (msg.x.pingid < 100) {
                    // ref: server uses PINGWEAVE_WRID_SEND
                    spdlog::get(logname)->error(
                        "Pingid must be large but given: {}", msg.x.pingid);
                    throw std::runtime_error("pingid must be large");
                }

                // Before SEND, record the start time
                if (clock_gettime(CLOCK_MONOTONIC, &var_ts) == -1) {
                    spdlog::get(logname)->error(
                        "Failed to run clock_gettime()");
                    throw std::runtime_error("clock_gettime is failed");
                }
                var_time = var_ts.tv_sec * 1000000000LL + var_ts.tv_nsec;

                // insert to table
                if (!ping_table->insert(msg.x.pingid,
                                        {msg.x.pingid, msg.x.qpn, msg.x.gid,
                                         dst_ip, var_time, 0, 0})) {
                    spdlog::get(logname)->warn(
                        "Failed to insert pingid {} into table.", msg.x.pingid);
                }

                // send to target
                spdlog::get(logname)->debug(
                    "SEND post with message (pingid: {}, rx's qpn: {}, rx's "
                    "gid: {}), send time: {}",
                    msg.x.pingid, msg.x.qpn, parsed_gid(&msg.x.gid), var_time);
                if (post_send(ctx_tx, dst_addr, msg.raw, sizeof(ping_msg_t),
                              msg.x.pingid)) {
                    if (check_log(ctx_tx->log_msg)) {
                        spdlog::get(logname)->error(ctx_tx->log_msg);
                    }
                    throw std::runtime_error("SEND post is failed");
                }
            }

            /**
             * IMPORTANT: Here we use non-blocking polling
             * and do not use event-driven polling.
             */
            struct ibv_poll_cq_attr attr = {};
            struct ibv_wc wc = {};
            int end_flag = false;
            if (ctx_tx->rnic_hw_ts) {  // extension
                // initialize polling CQ in case of HW timestamp usage
                ret = ibv_start_poll(ctx_tx->cq_s.cq_ex, &attr);
                while (!ret) {  // ret == 0 if success
                    end_flag = true;

                    /* do something */
                    wc = {0};
                    wc.status = ctx_tx->cq_s.cq_ex->status;  // status
                    wc.wr_id = ctx_tx->cq_s.cq_ex->wr_id;    // pingid
                    wc.opcode = ibv_wc_read_opcode(ctx_tx->cq_s.cq_ex);
                    cqe_time = ibv_wc_read_completion_ts(ctx_tx->cq_s.cq_ex);

                    if (wc.status == IBV_WC_SUCCESS) {
                        if (wc.opcode == IBV_WC_SEND) {
                            spdlog::get(logname)->debug(
                                "[CQE] SEND (wr_id: {})", wc.wr_id);

                            // update cqe time
                            spdlog::get(logname)->debug(
                                "-> update cqe_time to {}", cqe_time);
                            if (!ping_table->update_time_cqe(wc.wr_id,
                                                             cqe_time)) {
                                spdlog::get(logname)->error(
                                    "update_time_cqe is failed.");
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
                    ret = ibv_next_poll(ctx_tx->cq_s.cq_ex);
                }
                if (end_flag) {
                    ibv_end_poll(ctx_tx->cq_s.cq_ex);
                }

            } else {  // original
                ret = ibv_poll_cq(pingweave_cq(ctx_tx), 1, &wc);
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
                            spdlog::get(logname)->debug(
                                "[CQE] SEND (wr_id: {})", wc.wr_id);

                            // update cqe time
                            spdlog::get(logname)->debug(
                                "-> update cqe_time to {}", cqe_time);
                            if (!ping_table->update_time_cqe(wc.wr_id,
                                                             cqe_time)) {
                                spdlog::get(logname)->error(
                                    "update_time_cqe is failed.");
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
                    ret = ibv_poll_cq(pingweave_cq(ctx_tx), 1, &wc);
                }
            }

            std::this_thread::sleep_for(std::chrono::microseconds(10));
        }
    } catch (const std::exception& e) {
        spdlog::get(logname)->error("TX thread exits unexpectedly.");
        throw;  // Propagate exception
    }
}

void rdma_client(const std::string& ipv4) {
    // init logger
    spdlog::drop_all();
    const std::string logname = "rdma_client_" + ipv4;
    auto logger = initialize_custom_logger(logname, LOG_LEVEL_CLIENT);

    // ping table
    PingInfoMap ping_table(1);

    // RDMA context
    struct pingweave_context ctx_tx, ctx_rx;
    if (make_ctx(&ctx_tx, ipv4, logname, false)) {
        logger->error("Failed to make TX device info: {}", ipv4);
        throw std::runtime_error("make_ctx is failed.");
    }

    if (make_ctx(&ctx_rx, ipv4, logname, true)) {
        logger->error("Failed to make RX device info: {}", ipv4);
        throw std::runtime_error("make_ctx is failed.");
    }

    // Start TX thread
    std::thread tx_thread(client_tx_thread, ipv4, logname, &ping_table, &ctx_tx,
                          ctx_rx.gid, ctx_rx.qp->qp_num);

    /*********************************************************************/

    spdlog::get(logname)->info("Running main (RX) thread (pid: {})...",
                               getpid());

    // inter-process queue
    // ProducerQueue producer_queue("rdma", ipv4);

    // variables
    struct ping_info_t ping_info;
    union pong_msg_t pong_msg;
    uint64_t cqe_time, var_time;
    struct timespec cqe_ts, var_ts;

    // post 1 RECV WR
    if (post_recv(&ctx_rx, RX_DEPTH, PINGWEAVE_WRID_RECV) < RX_DEPTH) {
        spdlog::get(logname)->warn("RECV post is failed.");
    }

    // register an event alarm of cq
    if (ibv_req_notify_cq(pingweave_cq(&ctx_rx), 0)) {
        spdlog::get(logname)->error("Couldn't register CQE notification");
        throw std::runtime_error("Couldn't register CQE notification");
    }

    // Polling loop
    while (true) {
        /**
         * IMPORTANT: Event-driven polling via completion channel.
         **/
        spdlog::get(logname)->debug("Wait polling RX RECV CQE...");

        struct ibv_cq* ev_cq;
        void* ev_ctx;
        if (ibv_get_cq_event(ctx_rx.channel, &ev_cq, &ev_ctx)) {
            spdlog::get(logname)->error("Failed to get cq_event");
            throw std::runtime_error("Failed to get cq_event");
        }

        // ACK the CQ events
        ibv_ack_cq_events(pingweave_cq(&ctx_rx), 1);
        // check the cqe is from a correct CQ
        if (ev_cq != pingweave_cq(&ctx_rx)) {
            spdlog::get(logname)->error("CQ event for unknown CQ");
            throw std::runtime_error("CQ event for unknown CQ");
        }

        // re-register an event alarm of cq
        if (ibv_req_notify_cq(pingweave_cq(&ctx_rx), 0)) {
            spdlog::get(logname)->error("Couldn't register CQE notification");
            throw std::runtime_error("Couldn't register CQE notification");
        }
        /*------------------------------------------------------*/

        /** Case 1: CQE of PONG RECV
         * Get two timestamps:
         * (1) CQE of PONG at client -> network RTT
         * (2) current time -> client processing delay
         *
         * Case 2: CQE of ACK RECV
         * Record the server-side processing delay.
         * Finally, sends a message to producer_queue (for IPC)
         * Remove the pingID from ping_table.
         */
        struct ibv_poll_cq_attr attr = {};
        struct ibv_wc wc = {};
        int end_flag = false;
        int ret = 0;
        if (ctx_rx.rnic_hw_ts) {  // extension
            // initialize polling CQ in case of HW timestamp usage
            ret = ibv_start_poll(ctx_rx.cq_s.cq_ex, &attr);
            while (!ret) {  // ret == 0 if success
                end_flag = true;

                spdlog::get(logname)->trace(
                    "CQE event with wr_id: {}, status: {}",
                    ctx_rx.cq_s.cq_ex->wr_id, (int)ctx_rx.cq_s.cq_ex->status);

                /* do something */
                wc = {0};
                wc.status = ctx_rx.cq_s.cq_ex->status;  // status
                wc.wr_id = ctx_rx.cq_s.cq_ex->wr_id;    // pingid
                wc.opcode = ibv_wc_read_opcode(ctx_rx.cq_s.cq_ex);
                cqe_time = ibv_wc_read_completion_ts(ctx_rx.cq_s.cq_ex);

                if (clock_gettime(CLOCK_MONOTONIC, &var_ts) == -1) {
                    spdlog::get(logname)->error(
                        "Failed to run clock_gettime()");
                    throw std::runtime_error("clock_gettime is failed");
                }
                var_time = var_ts.tv_sec * 1000000000LL + var_ts.tv_nsec;

                // post 1 RECV WR
                if (post_recv(&ctx_rx, 1, PINGWEAVE_WRID_RECV) == 0) {
                    spdlog::get(logname)->warn("RECV post is failed.");
                }

                if (wc.status == IBV_WC_SUCCESS) {
                    if (wc.opcode == IBV_WC_RECV) {
                        spdlog::get(logname)->debug("[CQE] RECV (wr_id: {})",
                                                    wc.wr_id);

                        // parsing message
                        std::memcpy(&pong_msg, ctx_rx.buf + GRH_SIZE,
                                    sizeof(pong_msg_t));

                        // for debugging
                        spdlog::get(logname)->debug("-> pingID: {}, time:{}",
                                                    pong_msg.x.pingid,
                                                    pong_msg.x.server_delay);

                        // handle CQE
                        try {
                            if (pong_msg.x.opcode == PINGWEAVE_OPCODE_PONG) {
                                if (!ping_table.get(pong_msg.x.pingid,
                                                    ping_info)) {
                                    spdlog::get(logname)->error(
                                        "Failed to find {} in ping_table",
                                        pong_msg.x.pingid);
                                    throw;
                                }
                                spdlog::get(logname)->debug("-> PONG");
                                uint64_t time_send_el =
                                    calc_time_delta_with_bitwrap(
                                        ping_info.time_ping_send, var_time,
                                        UINT64_MAX);
                                uint64_t time_cqe_el =
                                    calc_time_delta_with_bitwrap(
                                        ping_info.time_ping_cqe, cqe_time,
                                        ctx_rx.completion_timestamp_mask);

                                // update client-delay, network-rtt.
                                if (!ping_table.update_time_send(
                                        pong_msg.x.pingid, time_send_el)) {
                                    spdlog::get(logname)->error(
                                        "update_time_send is failed.");
                                    throw;
                                }

                                if (!ping_table.update_time_cqe(
                                        pong_msg.x.pingid, time_cqe_el)) {
                                    spdlog::get(logname)->error(
                                        "update_time_send is failed.");
                                    throw;
                                }

                                // debugging
                                spdlog::get(logname)->debug(
                                    "-> client-delay: {}, network-rtt: {}",
                                    time_send_el, time_cqe_el);

                            } else if (pong_msg.x.opcode ==
                                       PINGWEAVE_OPCODE_ACK) {
                                assert(pong_msg.x.server_delay > 0);

                                spdlog::get(logname)->debug(
                                    "-> ACK with server-delay: {}",
                                    pong_msg.x.server_delay);

                                if (!ping_table.update_time_server(
                                        pong_msg.x.pingid,
                                        pong_msg.x.server_delay)) {
                                    spdlog::get(logname)->error(
                                        "update_time_server is failed.");
                                    // throw;
                                }

                                // for debugging
                                if (ping_table.get(pong_msg.x.pingid,
                                                   ping_info)) {
                                    auto ping_net_rtt =
                                        ping_info.time_ping_cqe -
                                        ping_info.time_server;
                                    spdlog::get(logname)->critical(
                                        "PING NETWORK RTT to {} is {}",
                                        ping_info.dstip, ping_net_rtt);
                                    // producer_queue.sendMessage(
                                    //     ipv4 + "," +
                                    //     std::to_string(ping_net_rtt));
                                } else {
                                    spdlog::get(logname)->error(
                                        "Final line error");
                                    throw;
                                }

                                // erase
                                if (!ping_table.remove(pong_msg.x.pingid)) {
                                    spdlog::get(logname)->warn(
                                        "Nothing to remove the id {}",
                                        pong_msg.x.pingid);
                                }
                            } else {
                                spdlog::get(logname)->error(
                                    "OPCODE must be either {} or {}, but {} "
                                    "received.",
                                    static_cast<int>(PINGWEAVE_OPCODE_PONG),
                                    static_cast<int>(PINGWEAVE_OPCODE_ACK),
                                    static_cast<int>(pong_msg.x.opcode));
                            }
                        } catch (const std::exception& e) {
                            spdlog::get(logname)->error("Error: {}", e.what());
                            throw std::runtime_error(e.what());
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
                ret = ibv_next_poll(ctx_rx.cq_s.cq_ex);
            }
            if (end_flag) {
                ibv_end_poll(ctx_rx.cq_s.cq_ex);
            }
        }

        // std::this_thread::sleep_for(std::chrono::microseconds(10));
    }
}
