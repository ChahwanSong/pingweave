#pragma once
#include "rdma_common.hpp"

// global variables
std::atomic<bool> server_rx_thread_running(false);

void server_rx_thread(const std::string& ipv4, const std::string& logname,
                      ServerQueue* server_queue) {
    server_rx_thread_running = true;
    int ret = 0;
    spdlog::get(logname)->info("Running RX thread...");

    struct pingweave_context ctx_rx;
    try {
        spdlog::get(logname)->info("Initializing server RX device for {}",
                                   ipv4);
        if (make_ctx(&ctx_rx, ipv4, logname, true, true)) {
            spdlog::get(logname)->error("Failed to make RX device info: {}",
                                        ipv4);
            throw std::runtime_error("Failed to make RX device info");
        }

        // post 1 RECV WR
        if (post_recv(&ctx_rx, 1, PINGWEAVE_WRID_RECV) == 0) {
            spdlog::get(logname)->warn("RECV post is failed.");
        }
        // register an event alarm of cq
        if (ibv_req_notify_cq(pingweave_cq(&ctx_rx), 0)) {
            spdlog::get(logname)->error("Couldn't register CQE notification");
            throw std::runtime_error("Couldn't register CQE notification");
        }

        // Polling loop
        uint64_t cqe_time = 0;
        struct timespec cqe_ts;
        struct ibv_wc wc = {};
        while (true) {
            spdlog::get(logname)->info("");
            spdlog::get(logname)->info(
                "Wait polling RX RECV CQE on device {}...",
                ctx_rx.context->device->name);

            // Event-driven polling via completion channel.
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

            struct ibv_poll_cq_attr attr = {};
            if (ctx_rx.rnic_hw_ts) {  // RNIC timestamping
                // initialize polling CQ in case of HW timestamp usage
                ret = ibv_start_poll(ctx_rx.cq_s.cq_ex, &attr);
                if (ret == ENOENT) {
                    spdlog::get(logname)->error(
                        "ibv_start_poll must have an entry.");
                    throw std::runtime_error(
                        "ibv_start_poll must have an entry.");
                }
            
                // if (ibv_next_poll(ctx_rx.cq_s.cq_ex) == ENOENT) {
                //     spdlog::get(logname)->error("CQE event does not exist");
                //     throw std::runtime_error("CQE event does not exist");
                // }
                wc = {0};
                wc.status = ctx_rx.cq_s.cq_ex->status;
                wc.wr_id = ctx_rx.cq_s.cq_ex->wr_id;
                wc.opcode = ibv_wc_read_opcode(ctx_rx.cq_s.cq_ex);
                wc.byte_len = ibv_wc_read_byte_len(ctx_rx.cq_s.cq_ex);
                cqe_time = ibv_wc_read_completion_ts(ctx_rx.cq_s.cq_ex);

                // finish polling CQ
                ibv_end_poll(ctx_rx.cq_s.cq_ex);

            } else {  // app-layer timestamping
                if (ibv_poll_cq(pingweave_cq(&ctx_rx), 1, &wc) != 1) {
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

            spdlog::get(logname)->debug("Received CQ Event!");

            if (wc.status == IBV_WC_SUCCESS) {
                if (wc.opcode == IBV_WC_RECV) {
                    spdlog::get(logname)->info("[CQE] RECV (wr_id: {})",
                                               wc.wr_id);
                    // post 1 RECV WR
                    if (post_recv(&ctx_rx, 1, PINGWEAVE_WRID_RECV) == 0) {
                        spdlog::get(logname)->warn("RECV post is failed.");
                    }

                    // GRH header parsing
                    struct ibv_grh* grh =
                        reinterpret_cast<struct ibv_grh*>(ctx_rx.buf);
                    char parsed_sgid[33];
                    inet_ntop(AF_INET6, &grh->sgid, parsed_sgid,
                              sizeof(parsed_sgid));
                    spdlog::get(logname)->debug("  -> from: {}", parsed_sgid);

                    // ping message parsing
                    union ping_msg_t ping_msg;
                    std::memcpy(&ping_msg, ctx_rx.buf + GRH_SIZE,
                                sizeof(ping_msg_t));

                    // msg to make response
                    union server_interval_msg_t internal_msg;
                    internal_msg.x.pingid = ping_msg.x.pingid;
                    internal_msg.x.qpn = ping_msg.x.qpn;
                    internal_msg.x.gid = grh->sgid;
                    internal_msg.x.time = 0;

                    char parsed_gid[33];
                    inet_ntop(AF_INET6, &internal_msg.x.gid, parsed_gid,
                              sizeof(parsed_gid));
                    spdlog::get(logname)->debug(
                        "  -> id : {}, gid: {}, qpn: {}, time: {}",
                        internal_msg.x.pingid, parsed_gid, internal_msg.x.qpn,
                        internal_msg.x.time);

                    if (!server_queue->try_enqueue(internal_msg)) {
                        spdlog::get(logname)->error(
                            "Failed to enqueue ping message: {}", parsed_gid);
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
                    "  RX WR failure - status: {}, opcode: {}",
                    ibv_wc_status_str(wc.status), static_cast<int>(wc.opcode));
            }

            // re-register an event alarm of cq
            if (ibv_req_notify_cq(pingweave_cq(&ctx_rx), 0)) {
                spdlog::get(logname)->error(
                    "Couldn't register CQE notification");
                throw std::runtime_error("Couldn't register CQE notification");
            }
        }
    } catch (const std::exception& e) {
        spdlog::get(logname)->error("RX thread exits unexpectedly.");
        ibv_end_poll(ctx_rx.cq_s.cq_ex);
        server_rx_thread_running = false;
        throw;  // Propagate exception
    }
}

void rdma_server(const std::string& ipv4) {
    // logger
    const std::string logname = "rdma_server_" + ipv4;
    auto logger = initialize_custom_logger(logname, LOG_LEVEL_SERVER);

    // RDMA context
    struct pingweave_context ctx_tx;

    // internal queue
    ServerQueue server_queue(QUEUE_SIZE);

    if (make_ctx(&ctx_tx, ipv4, logname, true, false)) {
        logger->error("Failed to make TX device info: {}", ipv4);
        raise;
    }

    // RX 스레드 초기화
    std::thread rx_thread(server_rx_thread, ipv4, logname, &server_queue);

    // 메인 스레드 루프 - 메시지 처리
    logger->info("Running main thread...");
    while (true) {
        server_interval_msg_t internal_msg;
        if (server_queue.try_dequeue(internal_msg)) {
            auto pingid = internal_msg.x.pingid;
            auto qpn = internal_msg.x.qpn;
            auto time = internal_msg.x.time;
            logger->info(
                "Internal queue received a msg - pingid: {}, qpn: {}, time: {}",
                pingid, qpn, time);
        }
        std::this_thread::sleep_for(std::chrono::microseconds(10));
    }

    // 스레드 종료 처리
    if (rx_thread.joinable()) {
        rx_thread.join();
    }
}