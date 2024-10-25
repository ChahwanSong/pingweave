#pragma once

#include "rdma_common.hpp"

// global variables
struct pingweave_context ctx_rx, ctx_tx;

void server_rx_thread(const std::string& ipv4, const std::string& logname) {
    spdlog::get(logname)->info("Running RX thread...");

    int ret;

    // initialize polling CQ in case of HW timestamp usage
    struct ibv_poll_cq_attr attr = {};
    if (ctx_rx.rnic_hw_ts) {
        ret = ibv_start_poll(ctx_rx.cq_s.cq_ex, &attr);
        assert(ret == ENOENT);  // should be nothing
    }

    // post 1 RECV WR
    if (post_recv(&ctx_rx, 1, PINGWEAVE_WRID_RECV) == 0) {
        spdlog::get(logname)->warn("RECV post is failed.");
    }

    spdlog::get(logname)->info("Wait polling RX RECV CQE of device {}...",
                               ctx_rx.context->device->name);

    // Polling loop
    uint64_t cqe_time = 0;
    struct timespec cqe_ts;
    struct ibv_wc wc = {};
    while (true) {
        // event signal (via completion channel)
        struct ibv_cq* ev_cq;
        void* ev_ctx;
        if (ibv_get_cq_event(ctx_rx.channel, &ev_cq, &ev_ctx)) {
            spdlog::get(logname)->error("Failed to get cq_event");
            break;
        }
        // save buffer content
        ibv_ack_cq_events(pingweave_cq(&ctx_rx), 1);

        if (ev_cq != pingweave_cq(&ctx_rx)) {
            spdlog::get(logname)->error("CQ event for unknown CQ");
            break;
        }
        if (ibv_req_notify_cq(pingweave_cq(&ctx_rx), 0)) {
            spdlog::get(logname)->error("Couldn't request CQ notification");
            break;
        }

        struct pingweave_addr src_addr;

        // poll -> CQE
        if (ctx_rx.rnic_hw_ts) {  // extension
            if (ibv_next_poll(ctx_rx.cq_s.cq_ex) == ENOENT) {
                spdlog::get(logname)->error("CQE event does not exist");
                break;
            }
            cqe_time = ibv_wc_read_completion_ts(ctx_rx.cq_s.cq_ex);
        } else {  // original
            if (ibv_poll_cq(pingweave_cq(&ctx_rx), 1, &wc) != 1) {
                spdlog::get(logname)->error("CQE poll receives nothing");
                break;
            }
            if (clock_gettime(CLOCK_MONOTONIC, &cqe_ts) == -1) {
                spdlog::get(logname)->error("Failed to run clock_gettime()");
                break;
            }
            cqe_time = cqe_ts.tv_sec * 1000000000LL + cqe_ts.tv_nsec;
        }

        wc = {0};
        wc.status = ctx_rx.cq_s.cq_ex->status;
        wc.wr_id = ctx_rx.cq_s.cq_ex->wr_id;
        wc.opcode = ctx_rx.cq_s.cq_ex->read_opcode(ctx_rx.cq_s.cq_ex);

        if (wc.status == IBV_WC_SUCCESS && wc.opcode == IBV_WC_RECV) {
            spdlog::get(logname)->info("  [CQE] RECV (wr_id: {})\n", wc.wr_id);
            spdlog::get(logname)->info("Server received {} bytes. Buffer: {}\n",
                                       wc.byte_len, ctx_rx.buf + GRH_SIZE);

            // format: <opcode, pingID,
        }

        // get the client address and msg to TX (main) thread
        std::this_thread::sleep_for(
            std::chrono::seconds(1));  // Simulate polling delay
    }
}

void rdma_server(const std::string& ipv4) {
    const std::string logname = "rdma_server_" + ipv4;
    auto logger = initialize_custom_logger(logname, LOG_LEVEL_SERVER);

    logger->info("Initializing Server RX/TX device for {}", ipv4);
    if (initialize_ctx(&ctx_rx, ipv4, logname, true)) {
        logger->error("Failed to initialize RX device info: {}", ipv4);
        raise;
    }

    if (initialize_ctx(&ctx_tx, ipv4, logname, false)) {
        logger->error("Failed to initialize TX device info: {}", ipv4);
        raise;
    }

    // RX thread
    std::thread rx_thread(server_rx_thread, ipv4, logname);

    // Main thread loop
    logger->info("Running main thread...");
    while (true) {
        logger->info("Main thread: Waiting 1 second...");
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }

    // Join the RX thread
    rx_thread.join();
}
