// #pragma once
// #include "rdma_common.hpp"

// void server_rx_thread(const std::string& ipv4, const std::string& logname,
//                       ServerInternalQueue* server_queue,
//                       struct pingweave_context* ctx_rx) {
//     spdlog::get(logname)->info("Running RX thread (pid: {})...", getpid());

//     int ret = 0;
//     uint64_t cqe_time = 0;
//     struct timespec cqe_ts;
//     struct ibv_wc wc = {};

//     try {
//         // post 1 RECV WR
//         if (post_recv(ctx_rx, 1, PINGWEAVE_WRID_RECV) == 0) {
//             spdlog::get(logname)->warn("RECV post is failed.");
//         }
//         // register an event alarm of cq
//         if (ibv_req_notify_cq(pingweave_cq(ctx_rx), 0)) {
//             spdlog::get(logname)->error("Couldn't register CQE
//             notification"); throw std::runtime_error("Couldn't register CQE
//             notification");
//         }

//         // Polling loop
//         while (true) {
//             spdlog::get(logname)->debug("Wait polling RX RECV CQE...");

//             /**
//              * IMPORTANT: Event-driven polling via completion channel.
//              **/
//             struct ibv_cq* ev_cq;
//             void* ev_ctx;
//             if (ibv_get_cq_event(ctx_rx->channel, &ev_cq, &ev_ctx)) {
//                 spdlog::get(logname)->error("Failed to get cq_event");
//                 throw std::runtime_error("Failed to get cq_event");
//             }

//             // ACK the CQ events
//             ibv_ack_cq_events(pingweave_cq(ctx_rx), 1);
//             // check the cqe is from a correct CQ
//             if (ev_cq != pingweave_cq(ctx_rx)) {
//                 spdlog::get(logname)->error("CQ event for unknown CQ");
//                 throw std::runtime_error("CQ event for unknown CQ");
//             }

//             // re-register an event alarm of cq
//             if (ibv_req_notify_cq(pingweave_cq(ctx_rx), 0)) {
//                 spdlog::get(logname)->error(
//                     "Couldn't register CQE notification");
//                 throw std::runtime_error("Couldn't register CQE
//                 notification");
//             }
//             /*------------------------------------------------------*/

//             struct ibv_poll_cq_attr attr = {};
//             if (ctx_rx->rnic_hw_ts) {  // RNIC timestamping
//                 // initialize polling CQ in case of HW timestamp usage
//                 ret = ibv_start_poll(ctx_rx->cq_s.cq_ex, &attr);
//                 if (ret) {
//                     spdlog::get(logname)->error("ibv_start_poll is failed:
//                     {}",
//                                                 ret);
//                     throw std::runtime_error("ibv_start_poll is failed.");
//                 }
//                 /**
//                  * ibv_next_poll gets the next item of batch (~16
//                  * items). (for further performance optimization)
//                  **/

//                 // if (ibv_next_poll(ctx_rx->cq_s.cq_ex) == ENOENT) {
//                 //     spdlog::get(logname)->error("CQE event does not
//                 exist");
//                 //     throw std::runtime_error("CQE event does not exist");
//                 // }
//                 wc = {0};
//                 wc.status = ctx_rx->cq_s.cq_ex->status;
//                 wc.wr_id = ctx_rx->cq_s.cq_ex->wr_id;
//                 wc.opcode = ibv_wc_read_opcode(ctx_rx->cq_s.cq_ex);
//                 cqe_time = ibv_wc_read_completion_ts(ctx_rx->cq_s.cq_ex);

//                 // finish polling CQ
//                 ibv_end_poll(ctx_rx->cq_s.cq_ex);

//             } else {  // app-layer timestamping
//                 if (ibv_poll_cq(pingweave_cq(ctx_rx), 1, &wc) != 1) {
//                     spdlog::get(logname)->error("CQE poll receives nothing");
//                     throw std::runtime_error("CQE poll receives nothing");
//                 }
//                 if (clock_gettime(CLOCK_MONOTONIC, &cqe_ts) == -1) {
//                     spdlog::get(logname)->error(
//                         "Failed to run clock_gettime()");
//                     throw std::runtime_error("Failed to run
//                     clock_gettime()");
//                 }
//                 cqe_time = cqe_ts.tv_sec * 1000000000LL + cqe_ts.tv_nsec;
//             }

//             // post 1 RECV WR
//             if (post_recv(ctx_rx, 1, PINGWEAVE_WRID_RECV) == 0) {
//                 spdlog::get(logname)->warn("RECV post is failed.");
//             }

//             if (wc.status == IBV_WC_SUCCESS) {
//                 if (wc.opcode == IBV_WC_RECV) {
//                     spdlog::get(logname)->debug("[CQE] RECV (wr_id: {})",
//                                                 wc.wr_id);

//                     // GRH header parsing (for debugging)
//                     struct ibv_grh* grh =
//                         reinterpret_cast<struct ibv_grh*>(ctx_rx->buf);
//                     spdlog::get(logname)->debug("  -> from: {}",
//                                                 parsed_gid(&grh->sgid));

//                     // ping message parsing
//                     union ping_msg_t ping_msg;
//                     std::memcpy(&ping_msg, ctx_rx->buf + GRH_SIZE,
//                                 sizeof(ping_msg_t));
//                     ping_msg.x.time = cqe_time;

//                     // for debugging
//                     spdlog::get(logname)->debug(
//                         "  -> id : {}, gid: {}, qpn: {}, time: {}",
//                         ping_msg.x.pingid, parsed_gid(&ping_msg.x.gid),
//                         ping_msg.x.qpn, ping_msg.x.time);

//                     if (!server_queue->try_enqueue(ping_msg)) {
//                         spdlog::get(logname)->error(
//                             "Failed to enqueue ping message");
//                         throw std::runtime_error(
//                             "Failed to enqueue ping message");
//                     }
//                 } else {
//                     spdlog::get(logname)->error(
//                         "SEND WC should not occur in Server RX thread");
//                     throw std::runtime_error(
//                         "SEND WC should not occur in Server RX thread");
//                 }
//             } else {
//                 spdlog::get(logname)->warn(
//                     "RX WR failure - status: {}, opcode: {}",
//                     ibv_wc_status_str(wc.status),
//                     static_cast<int>(wc.opcode));
//                 throw std::runtime_error("RX WR failure");
//             }
//         }
//     } catch (const std::exception& e) {
//         spdlog::get(logname)->error("RX thread exits unexpectedly.");
//         throw;  // Propagate exception
//     }
// }

// void rdma_server(const std::string& ipv4) {
//     // logger
//     spdlog::drop_all();
//     const std::string logname = "rdma_server_" + ipv4;
//     auto logger = initialize_custom_logger(logname, LOG_LEVEL_SERVER);

//     // internal queue
//     ServerInternalQueue server_queue(QUEUE_SIZE);

//     // RDMA context
//     struct pingweave_context ctx_tx, ctx_rx;
//     if (make_ctx(&ctx_tx, ipv4, logname, false)) {
//         logger->error("Failed to make TX device info: {}", ipv4);
//         throw std::runtime_error("make_ctx is failed.");
//     }

//     if (make_ctx(&ctx_rx, ipv4, logname, true)) {
//         logger->error("Failed to make RX device info: {}", ipv4);
//         throw std::runtime_error("make_ctx is failed.");
//     }
//     if (save_device_info(&ctx_rx)) {
//         logger->error(ctx_rx.log_msg);
//         throw std::runtime_error("save_device_info is failed.");
//     }

//     // Start RX thread
//     std::thread rx_thread(server_rx_thread, ipv4, logname, &server_queue,
//                           &ctx_rx);

//     /*********************************************************************/
//     // main thread loop - handle messages
//     logger->info("Running main (TX) thread (pid: {})...", getpid());

//     // Create the table (entry timeout = 1 second)
//     PingMsgMap pong_table(1);

//     // variables
//     union ping_msg_t ping_msg;
//     union pong_msg_t pong_msg;
//     uint64_t cqe_time, var_time;
//     struct timespec cqe_ts, var_ts;

//     while (true) {
//         /**
//          * SEND response (PONG) once it receives PING
//          * Memorize the ping ID -> {pingid, qpn, gid, time_ping}
//          */
//         ping_msg = {0};
//         if (server_queue.try_dequeue(ping_msg)) {
//             logger->debug(
//                 "Internal queue received a ping_msg - pingid: {}, qpn: {}, "
//                 "gid: {}, "
//                 "ping arrival time: {}",
//                 ping_msg.x.pingid, ping_msg.x.qpn,
//                 parsed_gid(&ping_msg.x.gid), ping_msg.x.time);

//             // (1) memorize
//             if (!pong_table.insert(ping_msg.x.pingid, ping_msg)) {
//                 spdlog::get(logname)->warn(
//                     "Failed to insert pingid {} into pong_table.",
//                     ping_msg.x.pingid);
//             }

//             // (2) send the response (what / where)
//             pong_msg = {0};
//             pong_msg.x.opcode = PINGWEAVE_OPCODE_PONG;
//             pong_msg.x.pingid = ping_msg.x.pingid;
//             pong_msg.x.server_delay = 0;

//             union rdma_addr dst_addr;
//             dst_addr.x.gid = ping_msg.x.gid;
//             dst_addr.x.qpn = ping_msg.x.qpn;

//             spdlog::get(logname)->debug(
//                 "SEND post with PONG message of pingid: {} to qpn: {}, gid:
//                 {}", pong_msg.x.pingid, dst_addr.x.qpn,
//                 parsed_gid(&dst_addr.x.gid));
//             if (post_send(&ctx_tx, dst_addr, pong_msg.raw,
//             sizeof(pong_msg_t),
//                           pong_msg.x.pingid)) {
//                 if (check_log(ctx_tx.log_msg)) {
//                     spdlog::get(logname)->error(ctx_tx.log_msg);
//                 }
//                 throw std::runtime_error("SEND PONG post is failed");
//             }
//         }

//         /**
//          * CQE capture
//          * IMPORTANT: we use non-blocking polling here.
//          *
//          * Case 1: CQE of Pong RECV -> ACK SEND
//          * Get CQE time of PONG SEND, and calculate delay
//          * Send ACK with the time and remove entry from pong_table
//          * wr_id is the ping ID
//          *
//          * Case 2: CQE of ACK -> ignore
//          * wr_id is PINGWEAVE_WRID_SEND
//          **/

//         struct ibv_poll_cq_attr attr = {};
//         struct ibv_wc wc = {};
//         int end_flag = false;
//         int ret;
//         if (ctx_tx.rnic_hw_ts) {  // extension
//             // initialize polling CQ in case of HW timestamp usage
//             ret = ibv_start_poll(ctx_tx.cq_s.cq_ex, &attr);

//             while (!ret) {  // ret == 0 if success
//                 end_flag = true;

//                 /* do something */
//                 wc = {0};
//                 wc.status = ctx_tx.cq_s.cq_ex->status;  // status
//                 wc.wr_id = ctx_tx.cq_s.cq_ex->wr_id;    // pingid
//                 wc.opcode = ibv_wc_read_opcode(ctx_tx.cq_s.cq_ex);
//                 cqe_time = ibv_wc_read_completion_ts(ctx_tx.cq_s.cq_ex);

//                 if (wc.status == IBV_WC_SUCCESS) {
//                     if (wc.opcode == IBV_WC_SEND) {
//                         spdlog::get(logname)->debug("[CQE] SEND (wr_id: {})",
//                                                     wc.wr_id);
//                         if (wc.wr_id == PINGWEAVE_WRID_SEND) {  // ACK -
//                         ignore
//                             spdlog::get(logname)->debug(
//                                 "-> CQE of ACK, so ignore this");
//                         } else {  // PONG CQE - send ACK
//                             spdlog::get(logname)->debug("-> PONG's pingID:
//                             {}",
//                                                         wc.wr_id);
//                             ping_msg = {0};
//                             if (pong_table.get(wc.wr_id, ping_msg)) {
//                                 // generate ACK message
//                                 pong_msg = {0};
//                                 pong_msg.x.opcode = PINGWEAVE_OPCODE_ACK;
//                                 pong_msg.x.pingid = wc.wr_id;
//                                 pong_msg.x.server_delay =
//                                     calc_time_delta_with_bitwrap(
//                                         ping_msg.x.time, cqe_time,
//                                         ctx_tx.completion_timestamp_mask);
//                                 spdlog::get(logname)->debug(
//                                     "SEND post with ACK message pingid: {} to
//                                     " "qpn: "
//                                     "{}, gid: {}, delay: {}",
//                                     pong_msg.x.pingid, ping_msg.x.qpn,
//                                     parsed_gid(&ping_msg.x.gid),
//                                     pong_msg.x.server_delay);

//                                 union rdma_addr dst_addr;
//                                 dst_addr.x.gid = ping_msg.x.gid;
//                                 dst_addr.x.qpn = ping_msg.x.qpn;

//                                 if (post_send(&ctx_tx, dst_addr,
//                                 pong_msg.raw,
//                                               sizeof(pong_msg_t),
//                                               PINGWEAVE_WRID_SEND)) {
//                                     if (check_log(ctx_tx.log_msg)) {
//                                         spdlog::get(logname)->error(
//                                             ctx_tx.log_msg);
//                                     }
//                                     throw std::runtime_error(
//                                         "SEND ACK post is failed");
//                                 }
//                             } else {
//                                 spdlog::get(logname)->warn(
//                                     "pingid {} entry does not exist
//                                     (expired?)", wc.wr_id);
//                             }
//                             // erase
//                             if (!pong_table.remove(wc.wr_id)) {
//                                 spdlog::get(logname)->warn(
//                                     "Nothing to remove the id {} from
//                                     timedMap", wc.wr_id);
//                             }
//                         }
//                     } else {
//                         spdlog::get(logname)->error(
//                             "Unexpected opcode: {}",
//                             static_cast<int>(wc.opcode));
//                         throw std::runtime_error("Unexpected opcode");
//                     }
//                 } else {
//                     spdlog::get(logname)->warn(
//                         "TX WR failure - status: {}, opcode: {}",
//                         ibv_wc_status_str(wc.status),
//                         static_cast<int>(wc.opcode));
//                     throw std::runtime_error("RX WR failure");
//                 }

//                 // next round
//                 ret = ibv_next_poll(ctx_tx.cq_s.cq_ex);
//             }
//             if (end_flag) {
//                 ibv_end_poll(ctx_tx.cq_s.cq_ex);
//             } else {  // no event to poll
//                 std::this_thread::sleep_for(std::chrono::microseconds(10));
//                 continue;
//             }
//         } else {  // original
//             ret = ibv_poll_cq(pingweave_cq(&ctx_tx), 1, &wc);

//             if (!ret) {  // no event to poll
//                 std::this_thread::sleep_for(std::chrono::microseconds(10));
//                 continue;
//             }

//             while (ret) {  // ret == 1 if success
//                 // get current time
//                 if (clock_gettime(CLOCK_MONOTONIC, &cqe_ts) == -1) {
//                     spdlog::get(logname)->error(
//                         "Failed to run clock_gettime()");
//                     throw std::runtime_error("clock_gettime is failed");
//                 }
//                 cqe_time = cqe_ts.tv_sec * 1000000000LL + cqe_ts.tv_nsec;

//                 if (wc.status == IBV_WC_SUCCESS) {
//                     if (wc.opcode == IBV_WC_SEND) {
//                         spdlog::get(logname)->debug("[CQE] SEND (wr_id: {})",
//                                                     wc.wr_id);
//                         if (wc.wr_id == PINGWEAVE_WRID_SEND) {  // ACK -
//                         ignore
//                             spdlog::get(logname)->debug(
//                                 "CQE of ACK, so ignore this");
//                         } else {
//                             spdlog::get(logname)->debug("-> PONG's pingID:
//                             {}",
//                                                         wc.wr_id);
//                             ping_msg = {0};
//                             if (pong_table.get(wc.wr_id, ping_msg)) {
//                                 // generate ACK message
//                                 pong_msg = {0};
//                                 pong_msg.x.opcode = PINGWEAVE_OPCODE_ACK;
//                                 pong_msg.x.pingid = wc.wr_id;
//                                 pong_msg.x.server_delay =
//                                     calc_time_delta_with_bitwrap(
//                                         ping_msg.x.time, cqe_time,
//                                         ctx_tx.completion_timestamp_mask);
//                                 spdlog::get(logname)->debug(
//                                     "SEND post with ACK message pingid: {} to
//                                     " "qpn: "
//                                     "{}, gid: {}, delay: {}",
//                                     pong_msg.x.pingid, ping_msg.x.qpn,
//                                     parsed_gid(&ping_msg.x.gid),
//                                     pong_msg.x.server_delay);

//                                 union rdma_addr dst_addr;
//                                 dst_addr.x.gid = ping_msg.x.gid;
//                                 dst_addr.x.qpn = ping_msg.x.qpn;

//                                 if (post_send(&ctx_tx, dst_addr,
//                                 pong_msg.raw,
//                                               sizeof(pong_msg_t),
//                                               PINGWEAVE_WRID_SEND)) {
//                                     if (check_log(ctx_tx.log_msg)) {
//                                         spdlog::get(logname)->error(
//                                             ctx_tx.log_msg);
//                                     }
//                                     throw std::runtime_error(
//                                         "SEND ACK post is failed");
//                                 }
//                             } else {
//                                 spdlog::get(logname)->warn(
//                                     "pingid {} entry does not exist
//                                     (expired?)", wc.wr_id);
//                             }
//                             // erase
//                             if (!pong_table.remove(wc.wr_id)) {
//                                 spdlog::get(logname)->warn(
//                                     "Nothing to remove the id {} from "
//                                     "pong_table",
//                                     wc.wr_id);
//                             }
//                         }
//                     } else {
//                         spdlog::get(logname)->error(
//                             "Unexpected opcode: {}",
//                             static_cast<int>(wc.opcode));
//                         throw std::runtime_error("Unexpected opcode");
//                     }
//                 } else {
//                     spdlog::get(logname)->warn(
//                         "TX WR failure - status: {}, opcode: {}",
//                         ibv_wc_status_str(wc.status),
//                         static_cast<int>(wc.opcode));
//                     throw std::runtime_error("RX WR failure");
//                 }

//                 // next round
//                 ret = ibv_poll_cq(pingweave_cq(&ctx_tx), 1, &wc);
//             }
//         }
//     }

//     // thread handling
//     if (rx_thread.joinable()) {
//         rx_thread.join();
//     }
// }

#pragma once
#include "rdma_common.hpp"

// 유틸리티 함수: CQ 이벤트 대기 및 처리
bool wait_for_cq_event(const std::string& logname,
                       struct pingweave_context* ctx) {
    struct ibv_cq* ev_cq;
    void* ev_ctx;

    if (ibv_get_cq_event(ctx->channel, &ev_cq, &ev_ctx)) {
        spdlog::get(logname)->error("Failed to get cq_event");
        return false;
    }

    // CQ 이벤트 ACK
    ibv_ack_cq_events(pingweave_cq(ctx), 1);

    // 올바른 CQ인지 확인
    if (ev_cq != pingweave_cq(ctx)) {
        spdlog::get(logname)->error("CQ event for unknown CQ");
        return false;
    }

    // CQ 이벤트 알림 재등록
    if (ibv_req_notify_cq(pingweave_cq(ctx), 0)) {
        spdlog::get(logname)->error("Couldn't register CQE notification");
        return false;
    }

    return true;
}

// 유틸리티 함수: CQE 폴링
bool poll_cq(const std::string& logname, struct pingweave_context* ctx,
             struct ibv_wc& wc, uint64_t& cqe_time) {
    int ret;
    if (ctx->rnic_hw_ts) {  // RNIC 타임스탬핑 사용
        struct ibv_poll_cq_attr attr = {};
        ret = ibv_start_poll(ctx->cq_s.cq_ex, &attr);
        if (ret) {
            spdlog::get(logname)->error("ibv_start_poll is failed: {}", ret);
            return false;
        }

        wc.status = ctx->cq_s.cq_ex->status;
        wc.wr_id = ctx->cq_s.cq_ex->wr_id;
        wc.opcode = ibv_wc_read_opcode(ctx->cq_s.cq_ex);
        cqe_time = ibv_wc_read_completion_ts(ctx->cq_s.cq_ex);

        ibv_end_poll(ctx->cq_s.cq_ex);
    } else {  // 애플리케이션 레벨 타임스탬핑 사용
        if (ibv_poll_cq(pingweave_cq(ctx), 1, &wc) != 1) {
            spdlog::get(logname)->error("CQE poll receives nothing");
            return false;
        }
        struct timespec cqe_ts;
        if (clock_gettime(CLOCK_MONOTONIC, &cqe_ts) == -1) {
            spdlog::get(logname)->error("Failed to run clock_gettime()");
            return false;
        }
        cqe_time = cqe_ts.tv_sec * 1000000000LL + cqe_ts.tv_nsec;
    }
    return true;
}

// 유틸리티 함수: RECV WR 포스팅
bool post_recv_wr(const std::string& logname, struct pingweave_context* ctx) {
    if (post_recv(ctx, 1, PINGWEAVE_WRID_RECV) == 0) {
        spdlog::get(logname)->warn("RECV post is failed.");
        return false;
    }
    return true;
}

// 유틸리티 함수: PING 메시지 처리
bool handle_ping_message(const std::string& logname,
                         struct pingweave_context* ctx, struct ibv_wc& wc,
                         uint64_t cqe_time, ServerInternalQueue* server_queue) {
    spdlog::get(logname)->debug("[CQE] RECV (wr_id: {})", wc.wr_id);

    // GRH 헤더 파싱 (디버깅 용도)
    struct ibv_grh* grh = reinterpret_cast<struct ibv_grh*>(ctx->buf);
    spdlog::get(logname)->debug("  -> from: {}", parsed_gid(&grh->sgid));

    // PING 메시지 파싱
    union ping_msg_t ping_msg;
    std::memcpy(&ping_msg, ctx->buf + GRH_SIZE, sizeof(ping_msg_t));
    ping_msg.x.time = cqe_time;

    // 디버깅 용도
    spdlog::get(logname)->debug("  -> id : {}, gid: {}, qpn: {}, time: {}",
                                ping_msg.x.pingid, parsed_gid(&ping_msg.x.gid),
                                ping_msg.x.qpn, ping_msg.x.time);

    if (!server_queue->try_enqueue(ping_msg)) {
        spdlog::get(logname)->error("Failed to enqueue ping message");
        return false;
    }
    return true;
}

// 유틸리티 함수: PONG CQE 처리
bool process_pong_cqe(const std::string& logname,
                      struct pingweave_context& ctx_tx, struct ibv_wc& wc,
                      uint64_t cqe_time, PingMsgMap& pong_table) {
    spdlog::get(logname)->debug("-> PONG's pingID: {}", wc.wr_id);
    union ping_msg_t ping_msg = {};
    if (pong_table.get(wc.wr_id, ping_msg)) {
        // ACK 메시지 생성
        union pong_msg_t pong_msg = {};
        pong_msg.x.opcode = PINGWEAVE_OPCODE_ACK;
        pong_msg.x.pingid = wc.wr_id;
        pong_msg.x.server_delay = calc_time_delta_with_bitwrap(
            ping_msg.x.time, cqe_time, ctx_tx.completion_timestamp_mask);
        spdlog::get(logname)->debug(
            "SEND post with ACK message pingid: {} to qpn: {}, gid: {}, delay: "
            "{}",
            pong_msg.x.pingid, ping_msg.x.qpn, parsed_gid(&ping_msg.x.gid),
            pong_msg.x.server_delay);

        union rdma_addr dst_addr;
        dst_addr.x.gid = ping_msg.x.gid;
        dst_addr.x.qpn = ping_msg.x.qpn;

        if (post_send(&ctx_tx, dst_addr, pong_msg.raw, sizeof(pong_msg_t),
                      PINGWEAVE_WRID_SEND)) {
            if (check_log(ctx_tx.log_msg)) {
                spdlog::get(logname)->error(ctx_tx.log_msg);
            }
            return false;
        }
    } else {
        spdlog::get(logname)->warn("pingid {} entry does not exist (expired?)",
                                   wc.wr_id);
    }
    // 테이블에서 엔트리 제거
    if (!pong_table.remove(wc.wr_id)) {
        spdlog::get(logname)->warn(
            "Nothing to remove the id {} from pong_table", wc.wr_id);
    }
    return true;
}

// 서버 RX 스레드
void server_rx_thread(const std::string& ipv4, const std::string& logname,
                      ServerInternalQueue* server_queue,
                      struct pingweave_context* ctx_rx) {
    spdlog::get(logname)->info("Running RX thread (pid: {})...", getpid());

    try {
        // 초기 RECV WR 포스팅
        if (!post_recv_wr(logname, ctx_rx)) {
            throw std::runtime_error("Initial RECV post failed");
        }

        // CQ 이벤트 알림 등록
        if (ibv_req_notify_cq(pingweave_cq(ctx_rx), 0)) {
            spdlog::get(logname)->error("Couldn't register CQE notification");
            throw std::runtime_error("Couldn't register CQE notification");
        }

        // 폴링 루프
        while (true) {
            spdlog::get(logname)->debug("Wait polling RX RECV CQE...");

            // CQ 이벤트 대기
            if (!wait_for_cq_event(logname, ctx_rx)) {
                throw std::runtime_error("Failed during CQ event waiting");
            }

            // CQE 폴링
            struct ibv_wc wc = {};
            uint64_t cqe_time = 0;
            if (!poll_cq(logname, ctx_rx, wc, cqe_time)) {
                throw std::runtime_error("Failed during CQ polling");
            }

            // 다음 RECV WR 포스팅
            if (!post_recv_wr(logname, ctx_rx)) {
                throw std::runtime_error("Failed to post next RECV WR");
            }

            // WC 상태 확인 및 처리
            if (wc.status == IBV_WC_SUCCESS) {
                if (wc.opcode == IBV_WC_RECV) {
                    if (!handle_ping_message(logname, ctx_rx, wc, cqe_time,
                                             server_queue)) {
                        throw std::runtime_error(
                            "Failed to handle PING message");
                    }
                } else {
                    spdlog::get(logname)->error("Unexpected opcode: {}",
                                                static_cast<int>(wc.opcode));
                    throw std::runtime_error("Unexpected opcode in RX thread");
                }
            } else {
                spdlog::get(logname)->warn(
                    "RX WR failure - status: {}, opcode: {}",
                    ibv_wc_status_str(wc.status), static_cast<int>(wc.opcode));
                throw std::runtime_error("RX WR failure");
            }
        }
    } catch (const std::exception& e) {
        spdlog::get(logname)->error("RX thread exits unexpectedly: {}",
                                    e.what());
        throw;  // 예외 전달
    }
}

// RDMA 서버 메인 함수
void rdma_server(const std::string& ipv4) {
    // 로거 초기화
    spdlog::drop_all();
    const std::string logname = "rdma_server_" + ipv4;
    auto logger = initialize_custom_logger(logname, LOG_LEVEL_SERVER);

    // 내부 큐 생성
    ServerInternalQueue server_queue(QUEUE_SIZE);

    // RDMA 컨텍스트 초기화
    struct pingweave_context ctx_tx, ctx_rx;
    if (make_ctx(&ctx_tx, ipv4, logname, false)) {
        logger->error("Failed to make TX device info: {}", ipv4);
        throw std::runtime_error("make_ctx is failed.");
    }

    if (make_ctx(&ctx_rx, ipv4, logname, true)) {
        logger->error("Failed to make RX device info: {}", ipv4);
        throw std::runtime_error("make_ctx is failed.");
    }
    if (save_device_info(&ctx_rx)) {
        logger->error(ctx_rx.log_msg);
        throw std::runtime_error("save_device_info is failed.");
    }

    // RX 스레드 시작
    std::thread rx_thread(server_rx_thread, ipv4, logname, &server_queue,
                          &ctx_rx);

    // 메인 스레드 루프 - 메시지 처리
    logger->info("Running main (TX) thread (pid: {})...", getpid());

    // 테이블 생성 (엔트리 타임아웃 = 1초)
    PingMsgMap pong_table(1);

    // 변수들
    union ping_msg_t ping_msg;
    union pong_msg_t pong_msg;
    uint64_t cqe_time;
    struct ibv_wc wc = {};
    int ret;

    while (true) {
        // 내부 큐에서 PING 메시지 수신 및 처리
        if (server_queue.try_dequeue(ping_msg)) {
            logger->debug(
                "Internal queue received a ping_msg - pingid: {}, qpn: {}, "
                "gid: {}, ping arrival time: {}",
                ping_msg.x.pingid, ping_msg.x.qpn, parsed_gid(&ping_msg.x.gid),
                ping_msg.x.time);

            // (1) 테이블에 저장
            if (!pong_table.insert(ping_msg.x.pingid, ping_msg)) {
                spdlog::get(logname)->warn(
                    "Failed to insert pingid {} into pong_table.",
                    ping_msg.x.pingid);
            }

            // (2) PONG 메시지 생성 및 전송
            pong_msg = {};
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

        // CQE 캡처 및 처리
        if (ctx_tx.rnic_hw_ts) {
            // RNIC 타임스탬핑 사용 시 CQE 폴링
            struct ibv_poll_cq_attr attr = {};
            ret = ibv_start_poll(ctx_tx.cq_s.cq_ex, &attr);
            bool has_events = false;

            while (!ret) {
                has_events = true;

                // WC 정보 추출
                wc.status = ctx_tx.cq_s.cq_ex->status;
                wc.wr_id = ctx_tx.cq_s.cq_ex->wr_id;
                wc.opcode = ibv_wc_read_opcode(ctx_tx.cq_s.cq_ex);
                cqe_time = ibv_wc_read_completion_ts(ctx_tx.cq_s.cq_ex);

                // WC 상태 확인 및 처리
                if (wc.status == IBV_WC_SUCCESS) {
                    if (wc.opcode == IBV_WC_SEND) {
                        if (wc.wr_id == PINGWEAVE_WRID_SEND) {
                            spdlog::get(logname)->debug(
                                "CQE of ACK, so ignore this");
                        } else {
                            if (process_pong_cqe(logname, ctx_tx, wc, cqe_time,
                                                 pong_table)) {
                                // 성공적으로 처리됨
                            } else {
                                throw std::runtime_error(
                                    "Failed to process PONG CQE");
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
                    throw std::runtime_error("TX WR failure");
                }

                // 다음 이벤트 폴링
                ret = ibv_next_poll(ctx_tx.cq_s.cq_ex);
            }
            if (has_events) {
                ibv_end_poll(ctx_tx.cq_s.cq_ex);
            } else {
                std::this_thread::sleep_for(std::chrono::microseconds(10));
                continue;
            }
        } else {
            // 애플리케이션 레벨 타임스탬핑 사용 시 CQE 폴링
            ret = ibv_poll_cq(pingweave_cq(&ctx_tx), 1, &wc);

            if (!ret) {
                std::this_thread::sleep_for(std::chrono::microseconds(10));
                continue;
            }

            while (ret) {
                struct timespec cqe_ts;
                if (clock_gettime(CLOCK_MONOTONIC, &cqe_ts) == -1) {
                    spdlog::get(logname)->error(
                        "Failed to run clock_gettime()");
                    throw std::runtime_error("clock_gettime is failed");
                }
                cqe_time = cqe_ts.tv_sec * 1000000000LL + cqe_ts.tv_nsec;

                if (wc.status == IBV_WC_SUCCESS) {
                    if (wc.opcode == IBV_WC_SEND) {
                        if (wc.wr_id == PINGWEAVE_WRID_SEND) {
                            spdlog::get(logname)->debug(
                                "CQE of ACK, so ignore this");
                        } else {
                            if (process_pong_cqe(logname, ctx_tx, wc, cqe_time,
                                                 pong_table)) {
                                // 성공적으로 처리됨
                            } else {
                                throw std::runtime_error(
                                    "Failed to process PONG CQE");
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
                    throw std::runtime_error("TX WR failure");
                }

                // 다음 이벤트 폴링
                ret = ibv_poll_cq(pingweave_cq(&ctx_tx), 1, &wc);
            }
        }
    }

    // RX 스레드 종료 처리
    if (rx_thread.joinable()) {
        rx_thread.join();
    }
}
