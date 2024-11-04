// #pragma once
// // #include "producer_queue.hpp"
// #include "rdma_common.hpp"
// #include "rdma_scheduler.hpp"

// void client_tx_thread(const std::string& ipv4, const std::string& logname,
//                       PingInfoMap* ping_table, struct pingweave_context*
//                       ctx_tx, const union ibv_gid& rx_gid, const uint32_t&
//                       rx_qpn) {
//     spdlog::get(logname)->info("Run TX thread after 1 second (pid: {})...",
//                                getpid());
//     std::this_thread::sleep_for(std::chrono::seconds(1));

//     // initialize pingid which monotonically increases;
//     // Note that it should be higher than 2 (PINGWEAVE_WRID_SEND)
//     // to distinguish between PONG and ACK at server
//     uint64_t it_pingid = PING_ID_INIT;

//     // variables
//     int ret = 0;
//     uint64_t cqe_time, var_time;
//     struct timespec cqe_ts, var_ts;

//     /**
//      * Schedule SEND PING with multiple destinations
//      * pinglist (ipv4) -> get the corresponding (ip, qpn, gid)
//      */
//     MsgScheduler scheduler(ipv4, logname);

//     try {
//         // Variables for msg scheduling
//         std::tuple<std::string, uint32_t, std::string> dst_next;
//         union rdma_addr dst_addr;
//         union ping_msg_t msg;
//         msg.x.qpn = rx_qpn;
//         msg.x.gid = rx_gid;
//         msg.x.time = 0;
//         msg.x.pingid = 0;

//         // SEND CQE message loop
//         while (true) {
//             /**
//              * SEND: Scheduler pops the next message to send.
//              */
//             if (scheduler.next(dst_next)) {  // if success
//                 std::string dst_ip = std::get<0>(dst_next);
//                 uint32_t dst_qpn = std::get<1>(dst_next);
//                 std::string dst_gid = std::get<2>(dst_next);

//                 // for debugging
//                 spdlog::get(logname)->debug("Target IP: {}, GID: {}, QPN:
//                 {}",
//                                             dst_ip,
//                                             parsed_gid(&dst_addr.x.gid),
//                                             dst_addr.x.qpn);
//                 /*----------------------------------------------*/

//                 // target address (i.e., destination)
//                 dst_addr = {0};
//                 dst_addr.x.qpn = dst_qpn;                           // qpn
//                 wire_gid_to_gid(dst_gid.c_str(), &dst_addr.x.gid);  // gid

//                 // message to send
//                 msg.x.pingid = it_pingid++;

//                 // sanity check: wr_id must be large enough
//                 if (msg.x.pingid < 100) {
//                     // ref: server uses PINGWEAVE_WRID_SEND
//                     spdlog::get(logname)->error(
//                         "Pingid must be large but given: {}", msg.x.pingid);
//                     throw std::runtime_error("pingid must be large");
//                 }

//                 // Before SEND, record the start time
//                 if (clock_gettime(CLOCK_MONOTONIC, &var_ts) == -1) {
//                     spdlog::get(logname)->error(
//                         "Failed to run clock_gettime()");
//                     throw std::runtime_error("clock_gettime is failed");
//                 }
//                 var_time = var_ts.tv_sec * 1000000000LL + var_ts.tv_nsec;

//                 // insert to table
//                 if (!ping_table->insert(msg.x.pingid,
//                                         {msg.x.pingid, msg.x.qpn, msg.x.gid,
//                                          dst_ip, var_time, 0, 0})) {
//                     spdlog::get(logname)->warn(
//                         "Failed to insert pingid {} into table.",
//                         msg.x.pingid);
//                 }

//                 // send to target
//                 spdlog::get(logname)->debug(
//                     "SEND post with message (pingid: {}, rx's qpn: {}, rx's "
//                     "gid: {}), send time: {}",
//                     msg.x.pingid, msg.x.qpn, parsed_gid(&msg.x.gid),
//                     var_time);
//                 if (post_send(ctx_tx, dst_addr, msg.raw, sizeof(ping_msg_t),
//                               msg.x.pingid)) {
//                     if (check_log(ctx_tx->log_msg)) {
//                         spdlog::get(logname)->error(ctx_tx->log_msg);
//                     }
//                     throw std::runtime_error("SEND post is failed");
//                 }
//             }

//             /**
//              * IMPORTANT: Here we use non-blocking polling
//              * and do not use event-driven polling.
//              */
//             struct ibv_poll_cq_attr attr = {};
//             struct ibv_wc wc = {};
//             int end_flag = false;
//             if (ctx_tx->rnic_hw_ts) {  // extension
//                 // initialize polling CQ in case of HW timestamp usage
//                 ret = ibv_start_poll(ctx_tx->cq_s.cq_ex, &attr);
//                 while (!ret) {  // ret == 0 if success
//                     end_flag = true;

//                     /* do something */
//                     wc = {0};
//                     wc.status = ctx_tx->cq_s.cq_ex->status;  // status
//                     wc.wr_id = ctx_tx->cq_s.cq_ex->wr_id;    // pingid
//                     wc.opcode = ibv_wc_read_opcode(ctx_tx->cq_s.cq_ex);
//                     cqe_time = ibv_wc_read_completion_ts(ctx_tx->cq_s.cq_ex);

//                     if (wc.status == IBV_WC_SUCCESS) {
//                         if (wc.opcode == IBV_WC_SEND) {
//                             spdlog::get(logname)->debug(
//                                 "[CQE] SEND (wr_id: {})", wc.wr_id);

//                             // update cqe time
//                             spdlog::get(logname)->debug(
//                                 "-> update cqe_time to {}", cqe_time);
//                             if (!ping_table->update_time_cqe(wc.wr_id,
//                                                              cqe_time)) {
//                                 spdlog::get(logname)->error(
//                                     "update_time_cqe is failed.");
//                             }
//                         } else {
//                             spdlog::get(logname)->error(
//                                 "Unexpected opcode: {}",
//                                 static_cast<int>(wc.opcode));
//                             throw std::runtime_error("Unexpected opcode");
//                         }
//                     } else {
//                         spdlog::get(logname)->warn(
//                             "TX WR failure - status: {}, opcode: {}",
//                             ibv_wc_status_str(wc.status),
//                             static_cast<int>(wc.opcode));
//                         throw std::runtime_error("RX WR failure");
//                     }

//                     // next round
//                     ret = ibv_next_poll(ctx_tx->cq_s.cq_ex);
//                 }
//                 if (end_flag) {
//                     ibv_end_poll(ctx_tx->cq_s.cq_ex);
//                 } else {  // no event to poll
//                     std::this_thread::sleep_for(std::chrono::microseconds(10));
//                     continue;
//                 }

//             } else {  // original
//                 ret = ibv_poll_cq(pingweave_cq(ctx_tx), 1, &wc);

//                 if (!ret) {  // no event to poll
//                     std::this_thread::sleep_for(std::chrono::microseconds(10));
//                     continue;
//                 }
//                 while (ret) {  // ret == 1 if success
//                     // get current time
//                     if (clock_gettime(CLOCK_MONOTONIC, &cqe_ts) == -1) {
//                         spdlog::get(logname)->error(
//                             "Failed to run clock_gettime()");
//                         throw std::runtime_error("clock_gettime is failed");
//                     }
//                     cqe_time = cqe_ts.tv_sec * 1000000000LL + cqe_ts.tv_nsec;

//                     if (wc.status == IBV_WC_SUCCESS) {
//                         if (wc.opcode == IBV_WC_SEND) {
//                             spdlog::get(logname)->debug(
//                                 "[CQE] SEND (wr_id: {})", wc.wr_id);

//                             // update cqe time
//                             spdlog::get(logname)->debug(
//                                 "-> update cqe_time to {}", cqe_time);
//                             if (!ping_table->update_time_cqe(wc.wr_id,
//                                                              cqe_time)) {
//                                 spdlog::get(logname)->error(
//                                     "update_time_cqe is failed.");
//                             }
//                         } else {
//                             spdlog::get(logname)->error(
//                                 "Unexpected opcode: {}",
//                                 static_cast<int>(wc.opcode));
//                             throw std::runtime_error("Unexpected opcode");
//                         }
//                     } else {
//                         spdlog::get(logname)->warn(
//                             "TX WR failure - status: {}, opcode: {}",
//                             ibv_wc_status_str(wc.status),
//                             static_cast<int>(wc.opcode));
//                         throw std::runtime_error("RX WR failure");
//                     }

//                     // next round
//                     ret = ibv_poll_cq(pingweave_cq(ctx_tx), 1, &wc);
//                 }
//             }
//         }
//     } catch (const std::exception& e) {
//         spdlog::get(logname)->error("TX thread exits unexpectedly.");
//         throw;  // Propagate exception
//     }
// }

// void rdma_client(const std::string& ipv4) {
//     // init logger
//     spdlog::drop_all();
//     const std::string logname = "rdma_client_" + ipv4;
//     auto logger = initialize_custom_logger(logname, LOG_LEVEL_CLIENT);

//     // ping table
//     PingInfoMap ping_table(1);

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

//     // Start TX thread
//     std::thread tx_thread(client_tx_thread, ipv4, logname, &ping_table,
//     &ctx_tx,
//                           ctx_rx.gid, ctx_rx.qp->qp_num);

//     /*********************************************************************/

//     spdlog::get(logname)->info("Running main (RX) thread (pid: {})...",
//                                getpid());

//     // inter-process queue
//     // ProducerQueue producer_queue("rdma", ipv4);

//     // variables
//     struct ping_info_t ping_info;
//     union pong_msg_t pong_msg;
//     uint64_t cqe_time, var_time;
//     struct timespec cqe_ts, var_ts;

//     // post 1 RECV WR
//     if (post_recv(&ctx_rx, RX_DEPTH, PINGWEAVE_WRID_RECV) < RX_DEPTH) {
//         spdlog::get(logname)->warn("RECV post is failed.");
//     }

//     // register an event alarm of cq
//     if (ibv_req_notify_cq(pingweave_cq(&ctx_rx), 0)) {
//         spdlog::get(logname)->error("Couldn't register CQE notification");
//         throw std::runtime_error("Couldn't register CQE notification");
//     }

//     // Polling loop
//     while (true) {
//         /**
//          * IMPORTANT: Event-driven polling via completion channel.
//          **/
//         spdlog::get(logname)->debug("Wait polling RX RECV CQE...");
//         struct ibv_cq* ev_cq;
//         void* ev_ctx;
//         if (ibv_get_cq_event(ctx_rx.channel, &ev_cq, &ev_ctx)) {
//             spdlog::get(logname)->error("Failed to get cq_event");
//             throw std::runtime_error("Failed to get cq_event");
//         }

//         // ACK the CQ events
//         ibv_ack_cq_events(pingweave_cq(&ctx_rx), 1);
//         // check the cqe is from a correct CQ
//         if (ev_cq != pingweave_cq(&ctx_rx)) {
//             spdlog::get(logname)->error("CQ event for unknown CQ");
//             throw std::runtime_error("CQ event for unknown CQ");
//         }

//         // re-register an event alarm of cq
//         if (ibv_req_notify_cq(pingweave_cq(&ctx_rx), 0)) {
//             spdlog::get(logname)->error("Couldn't register CQE
//             notification"); throw std::runtime_error("Couldn't register CQE
//             notification");
//         }
//         /*------------------------------------------------------*/

//         /** Case 1: CQE of PONG RECV
//          * Get two timestamps:
//          * (1) CQE of PONG at client -> network RTT
//          * (2) current time -> client processing delay
//          *
//          * Case 2: CQE of ACK RECV
//          * Record the server-side processing delay.
//          * Finally, sends a message to producer_queue (for IPC)
//          * Remove the pingID from ping_table.
//          */
//         struct ibv_poll_cq_attr attr = {};
//         struct ibv_wc wc = {};
//         int end_flag = false;
//         int ret = 0;
//         if (ctx_rx.rnic_hw_ts) {  // extension
//             // initialize polling CQ in case of HW timestamp usage
//             ret = ibv_start_poll(ctx_rx.cq_s.cq_ex, &attr);
//             while (!ret) {  // ret == 0 if success
//                 end_flag = true;

//                 spdlog::get(logname)->trace(
//                     "CQE event with wr_id: {}, status: {}",
//                     ctx_rx.cq_s.cq_ex->wr_id,
//                     (int)ctx_rx.cq_s.cq_ex->status);

//                 /* do something */
//                 wc = {0};
//                 wc.status = ctx_rx.cq_s.cq_ex->status;  // status
//                 wc.wr_id = ctx_rx.cq_s.cq_ex->wr_id;    // pingid
//                 wc.opcode = ibv_wc_read_opcode(ctx_rx.cq_s.cq_ex);
//                 cqe_time = ibv_wc_read_completion_ts(ctx_rx.cq_s.cq_ex);

//                 if (clock_gettime(CLOCK_MONOTONIC, &var_ts) == -1) {
//                     spdlog::get(logname)->error(
//                         "Failed to run clock_gettime()");
//                     throw std::runtime_error("clock_gettime is failed");
//                 }
//                 var_time = var_ts.tv_sec * 1000000000LL + var_ts.tv_nsec;

//                 // post 1 RECV WR
//                 if (post_recv(&ctx_rx, 1, PINGWEAVE_WRID_RECV) == 0) {
//                     spdlog::get(logname)->warn("RECV post is failed.");
//                 }

//                 if (wc.status == IBV_WC_SUCCESS) {
//                     if (wc.opcode == IBV_WC_RECV) {
//                         spdlog::get(logname)->debug("[CQE] RECV (wr_id: {})",
//                                                     wc.wr_id);

//                         // parsing message
//                         std::memcpy(&pong_msg, ctx_rx.buf + GRH_SIZE,
//                                     sizeof(pong_msg_t));

//                         // for debugging
//                         spdlog::get(logname)->debug("-> pingID: {}, time:{}",
//                                                     pong_msg.x.pingid,
//                                                     pong_msg.x.server_delay);

//                         // handle CQE
//                         try {
//                             if (pong_msg.x.opcode == PINGWEAVE_OPCODE_PONG) {
//                                 if (!ping_table.get(pong_msg.x.pingid,
//                                                     ping_info)) {
//                                     spdlog::get(logname)->error(
//                                         "Failed to find {} in ping_table",
//                                         pong_msg.x.pingid);
//                                     throw;
//                                 }
//                                 spdlog::get(logname)->debug("-> PONG");
//                                 uint64_t time_send_el =
//                                     calc_time_delta_with_bitwrap(
//                                         ping_info.time_ping_send, var_time,
//                                         UINT64_MAX);
//                                 uint64_t time_cqe_el =
//                                     calc_time_delta_with_bitwrap(
//                                         ping_info.time_ping_cqe, cqe_time,
//                                         ctx_rx.completion_timestamp_mask);

//                                 // update client-delay, network-rtt.
//                                 if (!ping_table.update_pong_info(
//                                         pong_msg.x.pingid, time_send_el,
//                                         time_cqe_el)) {
//                                     spdlog::get(logname)->error(
//                                         "Update info for PONG is failed - no
//                                         " "matched entry.");
//                                     throw;
//                                 }

//                                 // debugging
//                                 spdlog::get(logname)->debug(
//                                     "-> client-delay: {}, network-rtt: {}",
//                                     time_send_el, time_cqe_el);

//                             } else if (pong_msg.x.opcode ==
//                                        PINGWEAVE_OPCODE_ACK) {
//                                 assert(pong_msg.x.server_delay > 0);

//                                 spdlog::get(logname)->debug(
//                                     "-> ACK with server-delay: {}",
//                                     pong_msg.x.server_delay);

//                                 if (!ping_table.update_ack_info(
//                                         pong_msg.x.pingid,
//                                         pong_msg.x.server_delay)) {
//                                     spdlog::get(logname)->error(
//                                         "Update info for ACK is failed - no "
//                                         "matched entry.");
//                                     throw;
//                                 }

//                                 // for debugging
//                                 if (ping_table.get(pong_msg.x.pingid,
//                                                    ping_info)) {
//                                     auto ping_net_rtt =
//                                         ping_info.time_ping_cqe -
//                                         ping_info.time_server;
//                                     spdlog::get(logname)->critical(
//                                         "PING NETWORK RTT to {} is {}",
//                                         ping_info.dstip, ping_net_rtt);
//                                     // producer_queue.sendMessage(
//                                     //     ipv4 + "," +
//                                     //     std::to_string(ping_net_rtt));
//                                 } else {
//                                     spdlog::get(logname)->error(
//                                         "Final line error");
//                                     throw;
//                                 }

//                                 // erase
//                                 if (!ping_table.remove(pong_msg.x.pingid)) {
//                                     spdlog::get(logname)->warn(
//                                         "Nothing to remove the id {}",
//                                         pong_msg.x.pingid);
//                                 }
//                             } else {
//                                 spdlog::get(logname)->error(
//                                     "OPCODE must be either {} or {}, but {} "
//                                     "received.",
//                                     static_cast<int>(PINGWEAVE_OPCODE_PONG),
//                                     static_cast<int>(PINGWEAVE_OPCODE_ACK),
//                                     static_cast<int>(pong_msg.x.opcode));
//                             }
//                         } catch (const std::exception& e) {
//                             spdlog::get(logname)->error("Error: {}",
//                             e.what()); throw std::runtime_error(e.what());
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
//                 ret = ibv_next_poll(ctx_rx.cq_s.cq_ex);
//             }
//             if (end_flag) {
//                 ibv_end_poll(ctx_rx.cq_s.cq_ex);
//             } else {  // no event to poll
//                 std::this_thread::sleep_for(std::chrono::microseconds(10));
//             }
//         } else {  // original

//             ret = ibv_poll_cq(pingweave_cq(&ctx_rx), 1, &wc);
//             spdlog::get(logname)->debug("CQE event with wr_id: {}, status:
//             {}",
//                                         wc.wr_id, (int)wc.status);

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
//                 var_time = cqe_time;

//                 // post 1 RECV WR
//                 if (post_recv(&ctx_rx, 1, PINGWEAVE_WRID_RECV) == 0) {
//                     spdlog::get(logname)->warn("RECV post is failed.");
//                 }

//                 if (wc.status == IBV_WC_SUCCESS) {
//                     if (wc.opcode == IBV_WC_RECV) {
//                         spdlog::get(logname)->debug("[CQE] RECV (wr_id: {})",
//                                                     wc.wr_id);

//                         // parsing message
//                         std::memcpy(&pong_msg, ctx_rx.buf + GRH_SIZE,
//                                     sizeof(pong_msg_t));

//                         // for debugging
//                         spdlog::get(logname)->debug("-> pingID: {}, time:{}",
//                                                     pong_msg.x.pingid,
//                                                     pong_msg.x.server_delay);

//                         // handle CQE
//                         try {
//                             if (pong_msg.x.opcode == PINGWEAVE_OPCODE_PONG) {
//                                 if (!ping_table.get(pong_msg.x.pingid,
//                                                     ping_info)) {
//                                     spdlog::get(logname)->error(
//                                         "Failed to find {} in ping_table",
//                                         pong_msg.x.pingid);
//                                     throw;
//                                 }
//                                 spdlog::get(logname)->debug("-> PONG");
//                                 uint64_t time_send_el =
//                                     calc_time_delta_with_bitwrap(
//                                         ping_info.time_ping_send, var_time,
//                                         UINT64_MAX);
//                                 uint64_t time_cqe_el =
//                                     calc_time_delta_with_bitwrap(
//                                         ping_info.time_ping_cqe, cqe_time,
//                                         ctx_rx.completion_timestamp_mask);

//                                 // update client-delay, network-rtt.
//                                 if (!ping_table.update_pong_info(
//                                         pong_msg.x.pingid, time_send_el,
//                                         time_cqe_el)) {
//                                     spdlog::get(logname)->error(
//                                         "Update info for PONG is failed - no
//                                         " "matched entry.");
//                                     throw;
//                                 }

//                                 // debugging
//                                 spdlog::get(logname)->debug(
//                                     "-> client-delay: {}, network-rtt: {}",
//                                     time_send_el, time_cqe_el);

//                             } else if (pong_msg.x.opcode ==
//                                        PINGWEAVE_OPCODE_ACK) {
//                                 assert(pong_msg.x.server_delay > 0);

//                                 spdlog::get(logname)->debug(
//                                     "-> ACK with server-delay: {}",
//                                     pong_msg.x.server_delay);

//                                 if (!ping_table.update_ack_info(
//                                         pong_msg.x.pingid,
//                                         pong_msg.x.server_delay)) {
//                                     spdlog::get(logname)->error(
//                                         "Update info for ACK is failed - no "
//                                         "matched entry.");
//                                     throw;
//                                 }

//                                 // for debugging
//                                 if (ping_table.get(pong_msg.x.pingid,
//                                                    ping_info)) {
//                                     // NOTE: ignore server's process delay
//                                     auto ping_net_rtt =
//                                     ping_info.time_ping_cqe;
//                                     spdlog::get(logname)->critical(
//                                         "PING NETWORK RTT to {} is {}",
//                                         ping_info.dstip, ping_net_rtt);
//                                     // producer_queue.sendMessage(
//                                     //     ipv4 + "," +
//                                     //     std::to_string(ping_net_rtt));
//                                 } else {
//                                     spdlog::get(logname)->error(
//                                         "Final line error");
//                                     throw;
//                                 }

//                                 // erase
//                                 if (!ping_table.remove(pong_msg.x.pingid)) {
//                                     spdlog::get(logname)->warn(
//                                         "Nothing to remove the id {}",
//                                         pong_msg.x.pingid);
//                                 }
//                             } else {
//                                 spdlog::get(logname)->error(
//                                     "OPCODE must be either {} or {}, but {} "
//                                     "received.",
//                                     static_cast<int>(PINGWEAVE_OPCODE_PONG),
//                                     static_cast<int>(PINGWEAVE_OPCODE_ACK),
//                                     static_cast<int>(pong_msg.x.opcode));
//                             }
//                         } catch (const std::exception& e) {
//                             spdlog::get(logname)->error("Error: {}",
//                             e.what()); throw std::runtime_error(e.what());
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
//                 ret = ibv_poll_cq(pingweave_cq(&ctx_rx), 1, &wc);
//             }
//         }
//     }

//     // thread handling
//     if (tx_thread.joinable()) {
//         tx_thread.join();
//     }
// }

#pragma once
#include "rdma_common.hpp"
#include "rdma_scheduler.hpp"

// Function to get current timestamp
uint64_t get_current_timestamp() {
    struct timespec ts = {};
    if (clock_gettime(CLOCK_MONOTONIC, &ts) == -1) {
        throw std::runtime_error("Failed to call clock_gettime.");
    }
    return static_cast<uint64_t>(ts.tv_sec) * 1'000'000'000LL + ts.tv_nsec;
}

// Function to handle received messages
void handle_received_message(pingweave_context* ctx_rx,
                             const union pong_msg_t& pong_msg,
                             PingInfoMap* ping_table, uint64_t recv_time,
                             uint64_t cqe_time, const std::string& logname) {
    auto logger = spdlog::get(logname);
    ping_info_t ping_info = {};
    if (!ping_table->get(pong_msg.x.pingid, ping_info)) {
        logger->error("Cannot find information for ping ID {} in ping_table.",
                      pong_msg.x.pingid);
        return;
    }

    if (pong_msg.x.opcode == PINGWEAVE_OPCODE_PONG) {
        // Handle PONG message
        uint64_t client_delay = calc_time_delta_with_bitwrap(
            ping_info.time_ping_send, recv_time, UINT64_MAX);
        uint64_t network_rtt =
            calc_time_delta_with_bitwrap(ping_info.time_ping_cqe, cqe_time,
                                         ctx_rx->completion_timestamp_mask);
        ping_table->update_pong_info(pong_msg.x.pingid, client_delay,
                                     network_rtt);
        logger->debug(
            "Received PONG - ping ID: {}, client delay: {}, network RTT: {}",
            pong_msg.x.pingid, client_delay, network_rtt);
    } else if (pong_msg.x.opcode == PINGWEAVE_OPCODE_ACK) {
        // Handle ACK message
        ping_table->update_ack_info(pong_msg.x.pingid, pong_msg.x.server_delay);
        logger->debug("Received ACK - ping ID: {}, server delay: {}",
                      pong_msg.x.pingid, pong_msg.x.server_delay);

        // Final result output or storage
        uint64_t client_process_time =
            ping_info.time_ping_send - ping_info.time_ping_cqe;
        uint64_t server_process_time = pong_msg.x.server_delay;
        uint64_t network_rtt = ctx_rx->rnic_hw_ts ? ping_info.time_ping_cqe -
                                                        pong_msg.x.server_delay
                                                  : ping_info.time_ping_cqe;

        logger->info(
            "[Summary] Latency to {}: client {}, server {}, network: {}",
            ping_info.dstip, client_process_time, server_process_time,
            network_rtt);

        // Remove the entry from ping_table
        ping_table->remove(pong_msg.x.pingid);
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

    if (ctx_tx->rnic_hw_ts) {
        // Use extended CQ polling for hardware timestamping
        struct ibv_poll_cq_attr attr = {};
        int ret = ibv_start_poll(ctx_tx->cq_s.cq_ex, &attr);

        if (ret == 0) {
            // Process the current CQE
            wc.status = ctx_tx->cq_s.cq_ex->status;
            wc.wr_id = ctx_tx->cq_s.cq_ex->wr_id;
            wc.opcode = ibv_wc_read_opcode(ctx_tx->cq_s.cq_ex);
            cqe_time = ibv_wc_read_completion_ts(ctx_tx->cq_s.cq_ex);

            // End the polling session
            ibv_end_poll(ctx_tx->cq_s.cq_ex);

            has_cqe = true;
        } else {
            std::this_thread::sleep_for(std::chrono::microseconds(10));
        }
    } else {
        // Standard CQ polling
        int ret = ibv_poll_cq(pingweave_cq(ctx_tx), 1, &wc);
        if (ret == 1) {
            // Get current time
            cqe_time = get_current_timestamp();
            has_cqe = true;
        } else {
            // No CQE to process
            std::this_thread::sleep_for(std::chrono::microseconds(10));
        }
    }

    if (has_cqe) {
        if (wc.status == IBV_WC_SUCCESS && wc.opcode == IBV_WC_SEND) {
            logger->debug("Send completed (ping ID: {}), time: {}", wc.wr_id,
                          cqe_time);
            if (!ping_table->update_time_cqe(wc.wr_id, cqe_time)) {
                logger->error(
                    "Failed to update send completion time for ping ID {}.",
                    wc.wr_id);
            }
        } else {
            logger->warn("TX WR failure - status: {}, opcode: {}",
                         ibv_wc_status_str(wc.status),
                         static_cast<int>(wc.opcode));
        }
    }
}

// Function to process RX CQEs
void process_rx_cqe(pingweave_context* ctx_rx, PingInfoMap* ping_table,
                    const std::string& logname) {
    auto logger = spdlog::get(logname);

    struct ibv_wc wc = {};
    uint64_t cqe_time = 0, recv_time = 0;
    bool has_cqe = false;

    if (ctx_rx->rnic_hw_ts) {
        // Use extended CQ polling for hardware timestamping
        struct ibv_poll_cq_attr attr = {};
        int ret = ibv_start_poll(ctx_rx->cq_s.cq_ex, &attr);

        if (ret == 0) {
            // get recv time
            recv_time = get_current_timestamp();

            // Process the current CQE
            wc.status = ctx_rx->cq_s.cq_ex->status;
            wc.wr_id = ctx_rx->cq_s.cq_ex->wr_id;
            wc.opcode = ibv_wc_read_opcode(ctx_rx->cq_s.cq_ex);
            cqe_time = ibv_wc_read_completion_ts(ctx_rx->cq_s.cq_ex);

            // End the polling session
            ibv_end_poll(ctx_rx->cq_s.cq_ex);

            has_cqe = true;
        } else {
            std::this_thread::sleep_for(std::chrono::microseconds(10));
        }
    } else {
        // Standard CQ polling
        int ret = ibv_poll_cq(pingweave_cq(ctx_rx), 1, &wc);
        if (ret > 0) {
            // Get current time
            cqe_time = get_current_timestamp();
            recv_time = cqe_time;
            has_cqe = true;
        } else {
            // No CQE to process
            std::this_thread::sleep_for(std::chrono::microseconds(10));
        }
    }

    if (has_cqe) {
        if (wc.status == IBV_WC_SUCCESS && wc.opcode == IBV_WC_RECV) {
            // Parse the received message
            union pong_msg_t pong_msg = {};
            std::memcpy(&pong_msg, ctx_rx->buf + GRH_SIZE, sizeof(pong_msg_t));

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
                uint64_t send_time = get_current_timestamp();
                if (!ping_table->insert(msg.x.pingid,
                                        {msg.x.pingid, msg.x.qpn, msg.x.gid,
                                         dst_ip, send_time, 0, 0})) {
                    logger->warn("Failed to insert ping ID {} into ping_table.",
                                 msg.x.pingid);
                }

                // Send the PING message
                logger->debug(
                    "Sending PING message (ping ID: {}, QPN: {}, GID: {}), "
                    "time: {}",
                    msg.x.pingid, msg.x.qpn, parsed_gid(&msg.x.gid), send_time);
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

void rdma_client(const std::string& ipv4) {
    const std::string logname = "rdma_client_" + ipv4;
    auto logger = initialize_custom_logger(logname, LOG_LEVEL_CLIENT);

    PingInfoMap ping_table(1);  // Set timeout to 1 second

    // Initialize RDMA contexts
    pingweave_context ctx_tx = {};
    pingweave_context ctx_rx = {};
    if (initialize_contexts(ctx_tx, ctx_rx, ipv4, logname)) {
        throw std::runtime_error("Failed to initialize RDMA contexts.");
    }

    // Start the TX thread
    std::thread tx_thread(client_tx_thread, ipv4, logname, &ping_table, &ctx_tx,
                          ctx_rx.gid, ctx_rx.qp->qp_num);

    logger->info("Running main (RX) thread (PID: {})...", getpid());

    // Post RECV WRs and register CQ event notifications
    if (post_recv(&ctx_rx, RX_DEPTH, PINGWEAVE_WRID_RECV) < RX_DEPTH) {
        logger->warn("Failed to post RECV WRs.");
    }
    if (ibv_req_notify_cq(pingweave_cq(&ctx_rx), 0)) {
        throw std::runtime_error("Failed to register CQ event notifications.");
    }

    // Start the receive loop
    while (true) {
        try {
            // Process RX CQEs
            process_rx_cqe(&ctx_rx, &ping_table, logname);
        } catch (const std::exception& e) {
            logger->error("Exception in RX thread: {}", e.what());
        }
    }

    if (tx_thread.joinable()) {
        tx_thread.join();
    }
}