#pragma once
#include "producer_queue.hpp"
#include "rdma_common.hpp"

void rdma_client(const std::string& ipv4) {
    sleep(1);

    // init logger
    struct pingweave_context ctx_tmp;
    const std::string logname = "rdma_client_" + ipv4;
    auto logger = initialize_custom_logger(logname, LOG_LEVEL_CLIENT);

    logger->info("Initializing temporary client device for {}", ipv4);
    if (make_ctx(&ctx_tmp, ipv4, logname, false, false)) {
        logger->error("Failed to make RX device info: {}", ipv4);
        raise;
    }

    // init inter-process queue
    ProducerQueue producer_queue("rdma", ipv4);

    /**
     * TEST: generate a ping message to server RX
     **/
    int ret = 0;

    // post 1 RECV WR
    if (post_recv(&ctx_tmp, 1, PINGWEAVE_WRID_RECV) == 0) {
        spdlog::get(logname)->warn("RECV post is failed.");
    }

    // register an event alarm of cq
    if (ibv_req_notify_cq(pingweave_cq(&ctx_tmp), 0)) {
        spdlog::get(logname)->error("Couldn't register CQE notification");
        raise;
    }

    // 타겟 정보 읽어오기
    const std::string filepath =
        get_source_directory() + "/../local/10.200.200.3";
    union rdma_addr dst_addr;
    load_device_info(&dst_addr, filepath);

    // for debugging
    char parsed_gid[33];
    inet_ntop(AF_INET6, &dst_addr.x.gid, parsed_gid, sizeof(parsed_gid));
    logger->debug("Remote GID: {}, QPN: {}", parsed_gid, dst_addr.x.qpn);

    // send a message
    union ping_msg_t msg;
    msg.x.pingid = 123456;
    msg.x.qpn = ctx_tmp.qp->qp_num;

    struct ibv_wc wc = {};
    uint64_t cqe_time = 0;
    struct timespec cqe_ts;
    while (true) {
        ++msg.x.pingid;
        logger->debug("SEND post with pingid: {}, qpn: {}", msg.x.pingid,
                      msg.x.qpn);
        if (post_send(&ctx_tmp, dst_addr, msg.raw, sizeof(ping_msg_t),
                      PINGWEAVE_WRID_SEND)) {
            if (check_log(ctx_tmp.log_msg)) {
                spdlog::get(logname)->error(ctx_tmp.log_msg);
            }
            break;
        } else {
            logger->debug("-> successful.");
        }

        // Event-driven polling via completion channel.
        // Note that this is a **blocking** point.
        struct ibv_cq* ev_cq;
        void* ev_ctx;
        logger->debug("Waiting cq event...");
        if (ibv_get_cq_event(ctx_tmp.channel, &ev_cq, &ev_ctx)) {
            spdlog::get(logname)->error("Failed to get cq_event");
            break;
        }
        logger->debug("-> Gottcha!");
        // ACK the CQ events
        ibv_ack_cq_events(pingweave_cq(&ctx_tmp), 1);
        // check the cqe is from a correct CQ
        if (ev_cq != pingweave_cq(&ctx_tmp)) {
            spdlog::get(logname)->error("CQ event for unknown CQ");
            break;
        }

        // poll -> CQE
        struct ibv_poll_cq_attr attr = {};
        if (ctx_tmp.rnic_hw_ts) {  // extension
            // initialize polling CQ in case of HW timestamp usage
            ret = ibv_start_poll(ctx_tmp.cq_s.cq_ex, &attr);
            if (ret == ENOENT) {
                spdlog::get(logname)->error(
                    "ibv_start_poll must have an entry.");
                throw std::runtime_error("ibv_start_poll must have an entry.");
            }

            wc = {0};  // init
            wc.status = ctx_tmp.cq_s.cq_ex->status;
            wc.wr_id = ctx_tmp.cq_s.cq_ex->wr_id;
            wc.opcode = ibv_wc_read_opcode(ctx_tmp.cq_s.cq_ex);
            wc.byte_len = ibv_wc_read_byte_len(ctx_tmp.cq_s.cq_ex);
            cqe_time = ibv_wc_read_completion_ts(ctx_tmp.cq_s.cq_ex);

            // finish polling CQ
            ibv_end_poll(ctx_tmp.cq_s.cq_ex);
        } else {  // original
            if (ibv_poll_cq(pingweave_cq(&ctx_tmp), 1, &wc) != 1) {
                spdlog::get(logname)->error("CQE poll receives nothing");
                break;
            }
            if (clock_gettime(CLOCK_MONOTONIC, &cqe_ts) == -1) {
                spdlog::get(logname)->error("Failed to run clock_gettime()");
                break;
            }
            cqe_time = cqe_ts.tv_sec * 1000000000LL + cqe_ts.tv_nsec;
        }

        // re-register an event alarm of cq
        if (ibv_req_notify_cq(pingweave_cq(&ctx_tmp), 0)) {
            spdlog::get(logname)->error("Couldn't register CQE notification");
            break;
        }

        logger->debug("Sleep 0.1 seconds...");
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }
}
