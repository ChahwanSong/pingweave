#pragma once
#include "producer_queue.hpp"
#include "rdma_common.hpp"

void client_tx_thread(const std::string& ipv4, const std::string& logname,
                      ClientInternalQueue* client_queue,
                      struct pingweave_context* ctx_tx, const ibv_gid& rx_gid,
                      const uint32_t& rx_qpn) {
    spdlog::get(logname)->info("Running TX thread...");

    // initialize pingid which monotonically increases;
    // Note that it should be higher than 2 (PINGWEAVE_WRID_SEND)
    // to distinguish between PONG and ACK at server
    uint64_t c_pingid = 1000000000;

    // table to record
    TimedMap<ping_info_t> timedMap(1);

    // variables
    int ret = 0;
    uint64_t cqe_time, var_time;
    struct timespec cqe_ts, var_ts;

    /**
     * TODO: Schedule SEND PING with multiple destinations
     */

    try {
        /** TODO: update the code based on scheduling */
        const std::string filepath =
            get_source_directory() + "/../local/10.200.200.3";
        union rdma_addr dst_addr;
        load_device_info(&dst_addr, filepath);

        // for debugging
        spdlog::get(logname)->debug("Remote GID: {}, QPN: {}",
                                    parsed_gid(&dst_addr.x.gid),
                                    dst_addr.x.qpn);
        /*----------------------------------------------*/

        // // post 1 RECV WR
        // if (post_recv(ctx_tx, 1, PINGWEAVE_WRID_RECV) == 0) {
        //     spdlog::get(logname)->warn("RECV post is failed.");
        // }
        // // register an event alarm of cq
        // if (ibv_req_notify_cq(pingweave_cq(ctx_tx), 0)) {
        //     spdlog::get(logname)->error("Couldn't register CQE
        //     notification"); throw std::runtime_error("Couldn't register CQE
        //     notification");
        // }

        /** TODO: update the msg info */
        union ping_msg_t msg = {0};
        msg.x.pingid = c_pingid++;
        msg.x.qpn = rx_qpn;
        msg.x.gid = rx_gid;

        // Before SEND, record the start time
        if (clock_gettime(CLOCK_MONOTONIC, &var_ts) == -1) {
            spdlog::get(logname)->error("Failed to run clock_gettime()");
            throw std::runtime_error("clock_gettime is failed");
        }
        var_time = var_ts.tv_sec * 1000000000LL + var_ts.tv_nsec;
        if (!timedMap.insert(msg.x.pingid, {msg.x.pingid, msg.x.qpn, msg.x.gid,
                                            var_time, 0})) {
            spdlog::get(logname)->warn("Failed to insert pingid {} into table.",
                                       msg.x.pingid);
        }

        spdlog::get(logname)->debug(
            "SEND post with message (pingid: {}, rx's qpn: {}, rx's gid: {})",
            msg.x.pingid, msg.x.qpn, parsed_gid(&msg.x.gid));
        if (post_send(ctx_tx, dst_addr, msg.raw, sizeof(ping_msg_t),
                      msg.x.pingid)) {
            if (check_log(ctx_tx->log_msg)) {
                spdlog::get(logname)->error(ctx_tx->log_msg);
            }
            throw std::runtime_error("SEND post is failed");
        }

        // SEND CQE message loop
        while (true) {
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

                            // get
                            ping_info_t ping_info;
                            if (timedMap.get(wc.wr_id, ping_info)) {
                                // SEND cqe time
                                ping_info.time_ping_cqe = cqe_time;

                                // message queue
                                if (!client_queue->try_enqueue(ping_info)) {
                                    spdlog::get(logname)->error(
                                        "Failed to enqueue ping_info message");
                                    throw std::runtime_error(
                                        "Failed to enqueue ping_info message");
                                }
                            } else {
                                spdlog::get(logname)->warn(
                                    "pingid {} entry does not exist (expired?)",
                                    wc.wr_id);
                            }

                            // erase
                            if (!timedMap.remove(wc.wr_id)) {
                                spdlog::get(logname)->warn(
                                    "Nothing to remove the id {} from timedMap",
                                    wc.wr_id);
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

                            // get
                            ping_info_t ping_info;
                            if (timedMap.get(wc.wr_id, ping_info)) {
                                // SEND cqe time
                                ping_info.time_ping_cqe = cqe_time;

                                // message queue
                                if (!client_queue->try_enqueue(ping_info)) {
                                    spdlog::get(logname)->error(
                                        "Failed to enqueue ping_info message");
                                    throw std::runtime_error(
                                        "Failed to enqueue ping_info message");
                                }
                            } else {
                                spdlog::get(logname)->warn(
                                    "pingid {} entry does not exist (expired?)",
                                    wc.wr_id);
                            }

                            // erase
                            if (!timedMap.remove(wc.wr_id)) {
                                spdlog::get(logname)->warn(
                                    "Nothing to remove the id {} from timedMap",
                                    wc.wr_id);
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

            // spdlog::get(logname)->debug("Sleep 1 seconds...");
            std::this_thread::sleep_for(std::chrono::microseconds(100));
        }
    } catch (const std::exception& e) {
        spdlog::get(logname)->error("TX thread exits unexpectedly.");
        throw;  // Propagate exception
    }
}

void rdma_client(const std::string& ipv4) {
    sleep(1);

    // init logger
    spdlog::drop_all();
    const std::string logname = "rdma_client_" + ipv4;
    auto logger = initialize_custom_logger(logname, LOG_LEVEL_CLIENT);

    // internal queue
    ClientInternalQueue client_queue(QUEUE_SIZE);

    // RDMA context
    struct pingweave_context ctx_tx, ctx_rx;
    if (make_ctx(&ctx_tx, ipv4, logname, false, false)) {
        logger->error("Failed to make TX device info: {}", ipv4);
        raise;
    }

    if (make_ctx(&ctx_rx, ipv4, logname, false, true)) {
        logger->error("Failed to make RX device info: {}", ipv4);
        raise;
    }

    // Start TX thread
    std::thread tx_thread(client_tx_thread, ipv4, logname, &client_queue,
                          &ctx_tx, ctx_rx.gid, ctx_rx.qp->qp_num);

    /*********************************************************************/

    spdlog::get(logname)->info("Running main (RX) thread...");

    // inter-process queue
    ProducerQueue producer_queue("rdma", ipv4);

    TimedMap<ping_info_t> ping_table(1);

    while (true) {  // wait indefinitely
        ping_info_t ping_info;
        if (client_queue.try_dequeue(ping_info)) {
            logger->info(
                "Internal queue received a msg - pingid: {}, qpn (client rx): "
                "{}, gid: {}, "
                "time_ping_send: {}, time_ping_cqe: {}",
                ping_info.pingid, ping_info.qpn, parsed_gid(&ping_info.gid),
                ping_info.time_ping_send, ping_info.time_ping_cqe);
        }

        std::this_thread::sleep_for(std::chrono::microseconds(10));
    }
}
