#include "rdma_common.hpp"
#include "rdma_ping_info.hpp"
#include "rdma_ping_msg.hpp"

void server_process_rx_cqe(rdma_context* ctx_rx, RdmaServerQueue* server_queue,
                           std::shared_ptr<spdlog::logger> logger) {
    uint64_t cqe_time = 0;
    struct ibv_wc wc = {};
    int ret = 0;
    union rdma_pingmsg_t ping_msg = {};
    int num_cqes = 0;

    try {
        if (ctx_rx->rnic_hw_ts) {
            // Use extended CQ polling for hardware timestamping
            struct ibv_poll_cq_attr attr = {};
            bool has_events = false;
            ret = ibv_start_poll(ctx_rx->cq_s.cq_ex, &attr);

            while (!ret) {
                ++num_cqes;
                logger->trace("CQ Event loop {}", num_cqes);
                has_events = true;

                // Extract WC information
                wc.status = ctx_rx->cq_s.cq_ex->status;

                // if failure
                if (wc.status != IBV_WC_SUCCESS) {
                    logger->error("CQE RX error: {}",
                                  ibv_wc_status_str(wc.status));
                    ret = ibv_next_poll(ctx_rx->cq_s.cq_ex);
                    continue;
                }

                wc.opcode = ibv_wc_read_opcode(ctx_rx->cq_s.cq_ex);
                wc.wr_id = ctx_rx->cq_s.cq_ex->wr_id;
                cqe_time = ibv_wc_read_completion_ts(ctx_rx->cq_s.cq_ex);

                if (wc.opcode == IBV_WC_RECV) {
                    logger->debug("[CQE] RECV (wr_id: {})", wc.wr_id);

                    // Parse the received message
                    auto buf = ctx_rx->buf[wc.wr_id];
                    std::memcpy(&ping_msg, buf.addr + GRH_SIZE,
                                sizeof(rdma_pingmsg_t));
                    ping_msg.x.time = cqe_time;

                    // Parse GRH header (for debugging)
                    struct ibv_grh* grh = reinterpret_cast<struct ibv_grh*>(
                        ctx_rx->buf[wc.wr_id].addr);
                    logger->debug("-> from: {}", parsed_gid(&grh->sgid));
                    logger->debug(
                        "-> id: {}, gid: {}, lid: {}, qpn: {}, cqe_time: "
                        "{}",
                        ping_msg.x.pingid, parsed_gid(&ping_msg.x.gid),
                        ping_msg.x.lid, ping_msg.x.qpn, ping_msg.x.time);

                    // Post the next RECV WR
                    if (post_recv(ctx_rx, wc.wr_id, 1) == 0) {
                        logger->warn("Failed to repost the next RECV WR.");
                    }

                    // Handle the received message
                    if (!server_queue->try_enqueue(ping_msg)) {
                        logger->warn(
                            "Failed to enqueue ping message, pingid: {}, gid: "
                            "{}",
                            ping_msg.x.pingid, parsed_gid(&ping_msg.x.gid));
                    }
                } else {
                    logger->error("Unexpected opcode: {}",
                                  static_cast<int>(wc.opcode));
                    throw std::runtime_error(
                        "rx cqe - Unexpected opcode in RX thread");
                }
                ret = ibv_next_poll(ctx_rx->cq_s.cq_ex);
            }

            if (has_events) {
                // End the polling session
                ibv_end_poll(ctx_rx->cq_s.cq_ex);
            } else {  // nothing to poll
                logger->error("RX: CQE poll receives nothing");
                throw std::runtime_error("rx cqe - Failed during CQ polling");
            }
        } else {
            struct ibv_wc wc_array[BATCH_CQE];
            num_cqes = ibv_poll_cq(pingweave_cq(ctx_rx), BATCH_CQE, wc_array);

            if (num_cqes < 0) {
                logger->error("Failed to poll CQ");
                throw std::runtime_error("rx cqe - Failed to poll CQ");
            } else if (num_cqes == 0) {  // no completion
                logger->error("CQE poll receives nothing");
                throw std::runtime_error("rx cqe - Failed during CQ polling");
            }

            for (int i = 0; i < num_cqes; ++i) {
                struct ibv_wc& wc = wc_array[i];

                // if failure
                if (wc.status != IBV_WC_SUCCESS) {
                    logger->error("CQE RX error: {}",
                                  ibv_wc_status_str(wc.status));
                    continue;
                }

                if (wc.opcode == IBV_WC_RECV) {
                    logger->debug("[CQE] RECV (wr_id: {})", wc.wr_id);

                    // Parse the received message
                    auto buf = ctx_rx->buf[wc.wr_id];
                    std::memcpy(&ping_msg, buf.addr + GRH_SIZE,
                                sizeof(rdma_pingmsg_t));

                    cqe_time = get_current_timestamp_steady_ns();
                    ping_msg.x.time = cqe_time;

                    // Parse GRH header (for debugging)
                    struct ibv_grh* grh = reinterpret_cast<struct ibv_grh*>(
                        ctx_rx->buf[wc.wr_id].addr);
                    logger->debug("-> from: {}", parsed_gid(&grh->sgid));
                    logger->debug(
                        "-> id: {}, gid: {}, lid: {}, qpn: {}, time: {}",
                        ping_msg.x.pingid, parsed_gid(&ping_msg.x.gid),
                        ping_msg.x.lid, ping_msg.x.qpn, ping_msg.x.time);

                    // Post the next RECV WR
                    if (post_recv(ctx_rx, wc.wr_id, 1) == 0) {
                        logger->warn("Failed to repost the next RECV WR.");
                    }

                    // Handle the received message
                    if (!server_queue->try_enqueue(ping_msg)) {
                        logger->warn(
                            "Failed to enqueue ping msg, pingid: {}, gid: {}",
                            ping_msg.x.pingid, parsed_gid(&ping_msg.x.gid));
                        // throw std::runtime_error(
                        //     "Failed to handle PING message");
                    }
                } else {
                    logger->error("Unexpected opcode: {}",
                                  static_cast<int>(wc.opcode));
                    throw std::runtime_error(
                        "rx cqe - Unexpected opcode in RX thread");
                }
            }
        }

        // Acknowledge the CQ event
        ibv_ack_cq_events(pingweave_cq(ctx_rx), num_cqes);

        // Re-register for CQ event notifications
        if (ibv_req_notify_cq(pingweave_cq(ctx_rx), 0)) {
            logger->error("Couldn't register CQE notification");
            throw std::runtime_error(
                "rx cqe - Failed to post cqe request notification.");
        }

    } catch (const std::exception& e) {
        logger->error("RX CQE handler exits unexpectedly: {}", e.what());
        throw;  // Propagate exception
    }
}

// Utility function: Process PONG CQE
int server_process_pong_cqe(struct rdma_context* ctx_tx,
                            const struct ibv_wc& wc, const uint64_t& cqe_time,
                            PingMsgMap* ping_table,
                            std::shared_ptr<spdlog::logger> logger) {
    logger->debug("[CQE] PONG's pingID: {}, cqe_time: {}", wc.wr_id, cqe_time);
    union rdma_pingmsg_t ping_msg = {0};

    if (ping_table->get(wc.wr_id, ping_msg)) {
        // Create ACK message
        union rdma_pongmsg_t pong_msg = {};
        pong_msg.x.opcode = PINGWEAVE_OPCODE_ACK;
        pong_msg.x.pingid = wc.wr_id;
        /** NOTE: that infiniband CQE timestamp fluctuates around 2**33.
        So, we use bit wrap-around with 2**32 to ignore the effect. */
        pong_msg.x.server_delay = calc_time_delta_with_modulo(
            ping_msg.x.time, cqe_time, PINGWEAVE_TIME_CALC_MODULO, logger);
        logger->debug(
            "-> SEND post with ACK message pingid: {} to qpn: {}, gid: {}, "
            "lid: {}, delay: {}",
            pong_msg.x.pingid, ping_msg.x.qpn, parsed_gid(&ping_msg.x.gid),
            ping_msg.x.lid, pong_msg.x.server_delay);

        union rdma_addr dst_addr;
        dst_addr.x.gid = ping_msg.x.gid;
        dst_addr.x.lid = ping_msg.x.lid;
        dst_addr.x.qpn = ping_msg.x.qpn;

        // send PONG ACK
        if (IS_FAILURE(post_send(ctx_tx, dst_addr, pong_msg.raw,
                                 sizeof(rdma_pongmsg_t),
                                 wc.wr_id % ctx_tx->buf.size(),
                                 PINGWEAVE_WRID_PONG_ACK, logger))) {
            return PINGWEAVE_FAILURE;  // failed
        }
    } else {
        logger->warn("pingid {} entry does not exist at ping_table (expired?)",
                     wc.wr_id);
    }
    // Remove entry from table
    if (IS_FAILURE(ping_table->remove(wc.wr_id))) {
        logger->warn("Nothing to remove the id {} from ping_table", wc.wr_id);
    }

    // success
    return PINGWEAVE_SUCCESS;
}

void server_process_tx_cqe(rdma_context* ctx_tx, PingMsgMap* ping_table,
                           std::shared_ptr<spdlog::logger> logger) {
    uint64_t cqe_time = 0;
    struct ibv_wc wc = {};
    int ret = 0;
    int num_cqes = 0;

    try {
        if (ctx_tx->rnic_hw_ts) {
            // Use extended CQ polling for hardware timestamping
            struct ibv_poll_cq_attr attr = {};
            bool has_events = false;
            ret = ibv_start_poll(ctx_tx->cq_s.cq_ex, &attr);

            while (!ret) {
                ++num_cqes;
                logger->trace("CQ Event loop {}", num_cqes);
                has_events = true;

                // Extract WC information
                wc.status = ctx_tx->cq_s.cq_ex->status;

                // if failure
                if (wc.status != IBV_WC_SUCCESS) {
                    logger->error("CQE TX error: {}",
                                  ibv_wc_status_str(wc.status));
                    ret = ibv_next_poll(ctx_tx->cq_s.cq_ex);
                    continue;
                }

                wc.opcode = ibv_wc_read_opcode(ctx_tx->cq_s.cq_ex);
                wc.wr_id = ctx_tx->cq_s.cq_ex->wr_id;
                cqe_time = ibv_wc_read_completion_ts(ctx_tx->cq_s.cq_ex);

                if (wc.opcode == IBV_WC_SEND) {
                    logger->debug("[CQE] SEND (wr_id: {})", wc.wr_id);

                    // PONG ACK's CQE -> ignore
                    if (wc.wr_id == PINGWEAVE_WRID_PONG_ACK) {
                        logger->debug("[CQE] CQE of PONG ACK. cqe_time: {}.",
                                      cqe_time);
                        ret = ibv_next_poll(ctx_tx->cq_s.cq_ex);
                        continue;
                    }

                    // PONG's CQE ('wr_id' is the 'pingid')
                    if (IS_FAILURE(server_process_pong_cqe(ctx_tx, wc, cqe_time,
                                                ping_table, logger))) {
                        throw std::runtime_error(
                            "tx cqe - Failed to process PONG CQE");
                    }

                } else {
                    logger->error("Unexpected opcode: {}",
                                  static_cast<int>(wc.opcode));
                    throw std::runtime_error("tx cqe - Unexpected opcode");
                }

                // Poll next event
                ret = ibv_next_poll(ctx_tx->cq_s.cq_ex);
            }

            if (has_events) {
                ibv_end_poll(ctx_tx->cq_s.cq_ex);
            } else {
                logger->error("TX: CQE poll receives nothing");
                throw std::runtime_error("tx cqe - Failed during CQ polling");
                // std::this_thread::sleep_for(
                //     std::chrono::microseconds(SMALL_JITTERING_MICROSEC));
                // return;
            }
        } else {
            struct ibv_wc wc_array[BATCH_CQE];
            num_cqes = ibv_poll_cq(pingweave_cq(ctx_tx), BATCH_CQE, wc_array);

            if (num_cqes < 0) {
                logger->error("TX: Failed to poll CQ");
                throw std::runtime_error("tx cqe - Failed to poll CQ");
            } else if (num_cqes == 0) {  // no completion
                logger->error("TX: CQE poll receives nothing");
                throw std::runtime_error("tx cqe - Failed during CQ polling");
                // std::this_thread::sleep_for(
                //     std::chrono::microseconds(SMALL_JITTERING_MICROSEC));
                // return;
            }

            for (int i = 0; i < num_cqes; ++i) {
                struct ibv_wc& wc = wc_array[i];

                // if failure
                if (wc.status != IBV_WC_SUCCESS) {
                    logger->error("CQE TX error: {}",
                                  ibv_wc_status_str(wc.status));
                    continue;
                }

                if (wc.opcode == IBV_WC_SEND) {
                    logger->debug("[CQE] SEND (wr_id: {})", wc.wr_id);

                    // get CQE time
                    cqe_time = get_current_timestamp_steady_ns();

                    // PONG ACK's CQE -> ignore
                    if (wc.wr_id == PINGWEAVE_WRID_PONG_ACK) {
                        logger->debug("[CQE] CQE of PONG ACK. Do nothing.");
                        continue;
                    }

                    // PONG's CQE ('wr_id' is 'pingid')
                    if (IS_FAILURE(server_process_pong_cqe(ctx_tx, wc, cqe_time,
                                                ping_table, logger))) {
                        throw std::runtime_error(
                            "tx cqe - Failed to process PONG CQE");
                    }

                } else {
                    logger->error("Unexpected opcode: {}",
                                  static_cast<int>(wc.opcode));
                    throw std::runtime_error("tx cqe - Unexpected opcode");
                }
            }
        }

        // Acknowledge the CQ event
        ibv_ack_cq_events(pingweave_cq(ctx_tx), num_cqes);

        // Re-register for CQ event notifications
        if (ibv_req_notify_cq(pingweave_cq(ctx_tx), 0)) {
            logger->error("Couldn't register CQE notification");
            throw std::runtime_error(
                "tx cqe - Failed to post cqe request notification.");
        }
    } catch (const std::exception& e) {
        logger->error("TX CQE handler exits unexpectedly: {}", e.what());
        throw;  // Propagate exception
    }
}

// Server RX thread
void rdma_server_rx_thread(struct rdma_context* ctx_rx, const std::string& ipv4,
                           RdmaServerQueue* server_queue,
                           std::shared_ptr<spdlog::logger> logger) {
    logger->info("Running RX thread (Thread ID: {})...", get_thread_id());

    // RECV WR uses wr_id as a buffer index
    for (int i = 0; i < ctx_rx->buf.size(); ++i) {
        if (post_recv(ctx_rx, i, RX_DEPTH) != RX_DEPTH) {
            logger->error("Failed to post RECV WRs.");
            throw std::runtime_error(
                "rx thread - Failed to post RECV WR when initialization.");
        }
    }

    /** IMPORTANT: Use event-driven polling to reduce CPU overhead */
    // Register for CQ event notifications
    if (ibv_req_notify_cq(pingweave_cq(ctx_rx), 0)) {
        logger->error("Couldn't register CQE notification");
        throw std::runtime_error(
            "rx thread -  Couldn't register CQE notification");
    }

    try {
        // Polling loop
        while (true) {
            // Wait for the next CQE
            if (IS_FAILURE(wait_for_cq_event(ctx_rx, logger))) {
                throw std::runtime_error(
                    "rx thread -  Failed during CQ event waiting");
            }

            // Process RX CQEs
            server_process_rx_cqe(ctx_rx, server_queue, logger);
        }
    } catch (const std::exception& e) {
        logger->error("Exception in RX thread: {}", e.what());
        throw;  // Propagate exception
    }
}

void rdma_server_tx_cqe_thread(struct rdma_context* ctx_tx,
                               PingMsgMap* ping_table,
                               std::shared_ptr<spdlog::logger> logger) {
    // TX cQE thread loop - handle messages
    logger->info("Running TX CQE thread (Thread ID: {})...", get_thread_id());

    /** IMPORTANT: Use event-driven polling to reduce CPU overhead */
    // Register for CQ event notifications
    if (ibv_req_notify_cq(pingweave_cq(ctx_tx), 0)) {
        logger->error("Couldn't register CQE notification");
        throw std::runtime_error(
            "tx thread -  Couldn't register CQE notification");
    }

    try {
        while (true) {
            // Wait for the next CQE
            if (IS_FAILURE(wait_for_cq_event(ctx_tx, logger))) {
                throw std::runtime_error(
                    "tx thread -  Failed during CQ event waiting");
            }
            // Process TX CQEs
            server_process_tx_cqe(ctx_tx, ping_table, logger);
        }
    } catch (const std::exception& e) {
        logger->error("Exception in TX CQE thread: {}", e.what());
        throw;  // Propagate exception
    }
}

void rdma_server_tx_thread(struct rdma_context* ctx_tx, const std::string& ipv4,
                           RdmaServerQueue* server_queue,
                           PingMsgMap* ping_table,
                           std::shared_ptr<spdlog::logger> logger) {
    // TX thread loop - handle messages
    logger->info("Running TX thread (Thread ID: {})...", get_thread_id());

    // Variables
    union rdma_pingmsg_t ping_msg;
    union rdma_pongmsg_t pong_msg;
    uint64_t cqe_time;
    struct ibv_wc wc = {};
    int ret;
    union rdma_addr dst_addr;

    try {
        while (true) {
            // Receive and process PING message from internal queue
            if (server_queue->wait_dequeue_timed(
                    ping_msg,
                    std::chrono::milliseconds(WAIT_DEQUEUE_TIME_MS))) {
                logger->debug(
                    "Internal queue received a ping_msg - pingid: {}, qpn: "
                    "{}, gid: {}, lid: {}, ping arrival cqe_time: {}",
                    ping_msg.x.pingid, ping_msg.x.qpn,
                    parsed_gid(&ping_msg.x.gid), ping_msg.x.lid,
                    ping_msg.x.time);

                // (1) Store in table
                if (IS_FAILURE(ping_table->insert(ping_msg.x.pingid, ping_msg))) {
                    logger->warn("Failed to insert pingid {} into ping_table.",
                                 ping_msg.x.pingid);
                }

                // (2) Create and send PONG message
                pong_msg = {};
                pong_msg.x.opcode = PINGWEAVE_OPCODE_PONG;
                pong_msg.x.pingid = ping_msg.x.pingid;
                pong_msg.x.server_delay = 0;

                dst_addr.x.gid = ping_msg.x.gid;
                dst_addr.x.lid = ping_msg.x.lid;
                dst_addr.x.qpn = ping_msg.x.qpn;

                // send PONG message
                logger->debug(
                    "SEND post with PONG message of pingid: {} -> qpn: {}, "
                    "gid: {}, lid: {}",
                    pong_msg.x.pingid, dst_addr.x.qpn,
                    parsed_gid(&dst_addr.x.gid), dst_addr.x.lid);

                if (IS_FAILURE(post_send(ctx_tx, dst_addr, pong_msg.raw,
                                         sizeof(rdma_pongmsg_t),
                                         pong_msg.x.pingid % ctx_tx->buf.size(),
                                         pong_msg.x.pingid, logger))) {
                    throw std::runtime_error(
                        "tx thread - SEND PONG post failed");
                }
            }
        }
    } catch (const std::exception& e) {
        logger->error("Exception in TX thread: {}", e.what());
        throw;  // Propagate exception
    }
}

// RDMA server main function
void rdma_server(const std::string& ipv4, const std::string& protocol) {
    // Initialize logger
    const std::string server_logname = protocol + "_server_" + ipv4;
    enum spdlog::level::level_enum log_level_server;
    std::shared_ptr<spdlog::logger> server_logger;
    if (IS_FAILURE(get_log_config_from_ini(log_level_server,
                                           "logger_cpp_process_rdma_server"))) {
        throw std::runtime_error(
            "Failed to get a param 'logger_cpp_process_rdma_server'");
    } else {
        server_logger =
            initialize_logger(server_logname, DIR_LOG_PATH, log_level_server,
                              LOG_FILE_SIZE, LOG_FILE_EXTRA_NUM);
        server_logger->info("RDMA Server is running on pid {}", getpid());
    }

    // Create internal queue
    RdmaServerQueue server_queue(MSG_QUEUE_SIZE);

    // Create the table
    PingMsgMap ping_table(PINGWEAVE_TABLE_EXPIRY_TIME_RDMA_MS);

    // Initialize RDMA context
    rdma_context ctx_tx, ctx_rx;
    if (IS_FAILURE(initialize_contexts(ctx_tx, ctx_rx, ipv4, protocol,
                                       server_logger))) {
        throw std::runtime_error(
            "server main - Failed to initialize RDMA contexts.");
    }

    // Save file info for Server RX QP
    if (IS_FAILURE(save_device_info(&ctx_rx, server_logger))) {
        server_logger->error("Failed to save device info: {}", ipv4);
        throw std::runtime_error("server main - save_device_info failed.");
    }

    // Start RX thread
    std::thread rx_thread(rdma_server_rx_thread, &ctx_rx, ipv4, &server_queue,
                          server_logger);

    // Start TX CQE thread
    std::thread tx_cqe_thread(rdma_server_tx_cqe_thread, &ctx_tx, &ping_table,
                              server_logger);

    // Start TX thread
    std::thread tx_thread(rdma_server_tx_thread, &ctx_tx, ipv4, &server_queue,
                          &ping_table, server_logger);

    // termination
    if (rx_thread.joinable()) {
        rx_thread.join();
    }

    if (tx_cqe_thread.joinable()) {
        tx_cqe_thread.join();
    }

    if (tx_thread.joinable()) {
        tx_thread.join();
    }
}

void print_help() {
    std::cout
        << "Usage: rdma_server <IPv4 address> <protocol>\n"
        << "Arguments:\n"
        << "  IPv4 address   The target IPv4 address for RDMA server.\n"
        << "  protocol       The protocol name (should be 'roce' or 'ib').\n"
        << "Options:\n"
        << "  -h, --help     Show this help message.\n";
}

int main(int argc, char* argv[]) {
    if (argc != 3) {
        if (argc == 2 && (std::string(argv[1]) == "-h" ||
                          std::string(argv[1]) == "--help")) {
            print_help();
            return 0;
        }
        spdlog::error("Error: Invalid arguments.");
        print_help();
        return 1;
    }

    std::string ipv4 = argv[1];
    std::string protocol = argv[2];

    if (protocol != "roce" && protocol != "ib") {
        spdlog::error(
            "Error: Unsupported protocol. Only 'roce' or 'ib' is supported.");
        return 1;
    }

    try {
        rdma_server(ipv4, protocol);
    } catch (const std::exception& e) {
        spdlog::error("Exception occurred: {}", e.what());
        return 1;
    }

    return 0;
}