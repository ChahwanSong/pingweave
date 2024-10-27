#pragma once
#include "producer_queue.hpp"
#include "rdma_common.hpp"

void client_tx_thread(const std::string& ipv4, const std::string& logname,
                      InterThreadQueue* client_queue,
                      struct pingweave_context* ctx_tx, const ibv_gid& rx_gid,
                      const uint32_t& rx_qpn) {
    int ret = 0;
    spdlog::get(logname)->info("Running TX thread...");

    // initialize ping id which monotonically increases;
    uint32_t pingid = 1;

    try {
        // post 1 RECV WR
        if (post_recv(ctx_tx, 1, PINGWEAVE_WRID_RECV) == 0) {
            spdlog::get(logname)->warn("RECV post is failed.");
        }

        // register an event alarm of cq
        if (ibv_req_notify_cq(pingweave_cq(ctx_tx), 0)) {
            spdlog::get(logname)->error("Couldn't register CQE notification");
            raise;
        }

        /** TODO: update the code */
        const std::string filepath =
            get_source_directory() + "/../local/10.200.200.3";
        union rdma_addr dst_addr;
        load_device_info(&dst_addr, filepath);

        // for debugging
        spdlog::get(logname)->debug("Remote GID: {}, QPN: {}",
                                    parsed_gid(&dst_addr.x.gid),
                                    dst_addr.x.qpn);
        /*-----------------------*/

        struct ibv_wc wc = {};
        uint64_t cqe_time = 0;
        struct timespec cqe_ts;

        // send message loop
        while (true) {
            /** TODO: update the msg info */
            union ping_msg_t msg = {0};
            msg.x.pingid = pingid++;
            msg.x.qpn = rx_qpn;
            msg.x.gid = rx_gid;

            spdlog::get(logname)->debug(
                "SEND post with pingid: {}, qpn: {}, gid: {}", msg.x.pingid,
                msg.x.qpn, parsed_gid(&msg.x.gid));
            if (post_send(ctx_tx, dst_addr, msg.raw, sizeof(ping_msg_t),
                          PINGWEAVE_WRID_SEND)) {
                if (check_log(ctx_tx->log_msg)) {
                    spdlog::get(logname)->error(ctx_tx->log_msg);
                }
                break;
            } else {
                spdlog::get(logname)->debug("-> successful.");
            }

            // Event-driven polling via completion channel.
            // Note that this is a **blocking** point.
            struct ibv_cq* ev_cq;
            void* ev_ctx;
            spdlog::get(logname)->debug("Waiting cq event...");
            if (ibv_get_cq_event(ctx_tx->channel, &ev_cq, &ev_ctx)) {
                spdlog::get(logname)->error("Failed to get cq_event");
                break;
            }

            // ACK the CQ events
            ibv_ack_cq_events(pingweave_cq(ctx_tx), 1);
            // check the cqe is from a correct CQ
            if (ev_cq != pingweave_cq(ctx_tx)) {
                spdlog::get(logname)->error("CQ event for unknown CQ");
                break;
            }

            // poll -> CQE
            struct ibv_poll_cq_attr attr = {};
            if (ctx_tx->rnic_hw_ts) {  // extension
                // initialize polling CQ in case of HW timestamp usage
                ret = ibv_start_poll(ctx_tx->cq_s.cq_ex, &attr);
                if (ret == ENOENT) {
                    spdlog::get(logname)->error(
                        "ibv_start_poll must have an entry.");
                    throw std::runtime_error(
                        "ibv_start_poll must have an entry.");
                }

                wc = {0};  // init
                wc.status = ctx_tx->cq_s.cq_ex->status;
                wc.wr_id = ctx_tx->cq_s.cq_ex->wr_id;
                wc.opcode = ibv_wc_read_opcode(ctx_tx->cq_s.cq_ex);
                wc.byte_len = ibv_wc_read_byte_len(ctx_tx->cq_s.cq_ex);
                cqe_time = ibv_wc_read_completion_ts(ctx_tx->cq_s.cq_ex);

                // finish polling CQ
                ibv_end_poll(ctx_tx->cq_s.cq_ex);
            } else {  // original
                if (ibv_poll_cq(pingweave_cq(ctx_tx), 1, &wc) != 1) {
                    spdlog::get(logname)->error("CQE poll receives nothing");
                    break;
                }
                if (clock_gettime(CLOCK_MONOTONIC, &cqe_ts) == -1) {
                    spdlog::get(logname)->error(
                        "Failed to run clock_gettime()");
                    break;
                }
                cqe_time = cqe_ts.tv_sec * 1000000000LL + cqe_ts.tv_nsec;
            }

            // re-register an event alarm of cq
            if (ibv_req_notify_cq(pingweave_cq(ctx_tx), 0)) {
                spdlog::get(logname)->error(
                    "Couldn't register CQE notification");
                break;
            }

            spdlog::get(logname)->debug("Sleep 10 seconds...");
            std::this_thread::sleep_for(std::chrono::seconds(10));
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
    InterThreadQueue client_queue(QUEUE_SIZE);

    // inter-process queue
    ProducerQueue producer_queue("rdma", ipv4);

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

    while (true) {  // wait indefinitely
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }
}
