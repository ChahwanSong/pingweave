#pragma once

#include "ipc_producer.hpp"
#include "tcpudp_common.hpp"
#include "tcpudp_ping_info.hpp"
#include "tcpudp_scheduler.hpp"

void udp_client_tx_thread(struct udp_context* ctx_tx, const std::string& ipv4,
                          TcpUdpPinginfoMap* ping_table,
                          std::shared_ptr<spdlog::logger> logger) {
    logger->info("Running TX thread (Thread ID: {})...", get_thread_id());

    uint32_t ping_uid = 0;
    uint64_t time_sleep_us = 0;
    TcpUdpMsgScheduler scheduler(ipv4, "udp", logger);
    std::string dst_addr;

    try {
        while (true) {
            // Retrieve the next destination for sending
            if (scheduler.next(dst_addr, time_sleep_us)) {
                // Create a pingid
                auto pingid = make_pingid(ip2uint(ipv4), ping_uid++);

                // Record the send time
                if (!ping_table->insert(pingid, pingid, dst_addr)) {
                    logger->warn("Failed to insert ping ID {} into ping_table.",
                                 pingid);
                }

                // Send the PING message
                logger->debug("Sending PING message (ping ID:{}, dstip:{})",
                              pingid, dst_addr);

                if (send_udp_message(ctx_tx, dst_addr,
                                     PINGWEAVE_UDP_PORT_SERVER, pingid,
                                     logger)) {
                    // something went wrong
                    continue;
                }
            } else {
                // sleep until next ping schedule
                std::this_thread::sleep_for(
                    std::chrono::microseconds(time_sleep_us));
            }
        }
    } catch (const std::exception& e) {
        logger->error("Exception in TX thread: {}", e.what());
        throw;  // Propagate exception
    }
}

void udp_client_rx_thread(struct udp_context* ctx_rx,
                          TcpUdpPinginfoMap* ping_table,
                          std::shared_ptr<spdlog::logger> logger) {
    logger->info("Running RX thread (Thread ID: {})...", get_thread_id());

    try {
        while (true) {
            // Wait for the next UDP message event
            uint64_t pingid = 0;
            uint64_t recv_time_steady;
            std::string sender;
            if (receive_udp_message(ctx_rx, pingid, sender, recv_time_steady,
                                    logger)) {
                // something wrong
                continue;
            }

            if (!ping_table->update_pong_info(pingid, recv_time_steady)) {
                logger->warn("PONG ({}): No entry in ping_table", pingid);
                continue;
            }
        }
    } catch (const std::exception& e) {
        logger->error("Exception in RX thread: {}", e.what());
        throw;  // Propagate exception`
    }
}

void udp_client_result_thread(const std::string& ipv4,
                              const std::string& protocol,
                              TcpUdpClientQueue* client_queue,
                              std::shared_ptr<spdlog::logger> logger) {
    int report_interval_ms = 10000;
    if (get_int_param_from_ini(report_interval_ms,
                               "interval_report_ping_result_millisec")) {
        logger->error(
            "Failed to load report_interval parameter from pingwewave.ini. Use "
            "default - 10 seconds");
        report_interval_ms = 10000;
    }

    // dstip -> result history
    std::unordered_map<uint32_t, struct tcpudp_result_info_t> dstip2result;

    // msg from RX Thread
    struct tcpudp_result_msg_t result_msg;

    // result pointer
    struct tcpudp_result_info_t* info;

    // get controller address and port
    std::string controller_host;
    int controller_port;
    if (get_controller_info_from_ini(controller_host, controller_port)) {
        logger->error(
            "Exit the result thread - failed to load pingweave.ini file");
        throw;  // Propagate exception
    }

    // IPC - producer queue
    ProducerQueue ipc_producer(protocol, ipv4);

    // timer for report
    auto last_report_time = get_current_timestamp_steady_clock();

    /** RESULT: (dstip, #success, #failure, #weird, mean, max, p50, p95, p99) */
    try {
        while (true) {
            // fully-blocking with timeout
            if (client_queue->wait_dequeue_timed(
                    result_msg,
                    std::chrono::milliseconds(WAIT_DEQUEUE_TIME_MS))) {
                logger->debug("{}, {}, {}, {}, {}",
                              timestamp_ns_to_string(result_msg.time_ping_send),
                              uint2ip(result_msg.dstip), result_msg.pingid,
                              result_msg.network_delay, result_msg.result);
                // load a result
                info = &dstip2result[result_msg.dstip];
                if (info->ts_start == 0) {
                    info->ts_start = result_msg.time_ping_send;  // initialize
                }
                info->ts_end = result_msg.time_ping_send;

                if (result_msg.result == PINGWEAVE_RESULT_SUCCESS) {  // success
                    ++info->n_success;
                    info->network_delays.push_back(result_msg.network_delay);
                } else if (result_msg.result == PINGWEAVE_RESULT_FAILURE) {
                    ++info->n_failure;
                } else if (result_msg.result == PINGWEAVE_RESULT_WEIRD) {
                    ++info->n_weird;
                } else {
                    logger->error("Unknown type of result - {}, dstip:{}",
                                  result_msg.result, result_msg.dstip);
                    continue;
                }
            }

            // Check the interval for report
            auto current_time = get_current_timestamp_steady_clock();
            auto elapsed_time =
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    current_time - last_report_time)
                    .count();

            if (elapsed_time >= report_interval_ms) {
                // aggregated results
                std::string agg_result = "";

                for (auto& [dstip, result_info] : dstip2result) {
                    result_stat_t network_stat =
                        calc_result_stats(result_info.network_delays);

                    auto result = convert_tcpudp_result_to_str(
                        ipv4, uint2ip(dstip), result_info, network_stat);

                    // logging
                    logger->info(result);

                    // aggregate the results to one big string (unused now)
                    agg_result += result + "\n";
                }

                // result sending to agent_sender
                ipc_producer.writeMessage(agg_result);

                // clear the history
                dstip2result.clear();

                // update the last report time
                last_report_time = current_time;
            }
        }
    } catch (const std::exception& e) {
        logger->error("Exception in result_thread: {}", e.what());
        throw;
    }
}

void udp_client(const std::string& ipv4, const std::string& protocol) {
    // Start the RX thread
    const std::string client_logname = protocol + "_client_" + ipv4;
    enum spdlog::level::level_enum log_level_client;
    std::shared_ptr<spdlog::logger> client_logger;
    if (!get_log_config_from_ini(log_level_client,
                                 "logger_cpp_process_udp_client")) {
        client_logger =
            initialize_logger(client_logname, DIR_LOG_PATH, log_level_client,
                              LOG_FILE_SIZE, LOG_FILE_EXTRA_NUM);
        client_logger->info("UDP Client is running on pid {}", getpid());
    } else {
        throw std::runtime_error(
            "Failed to get a param 'logger_cpp_process_udp_client'");
    }

    // Inter-thread queue
    const std::string result_logname = protocol + "_" + ipv4;
    enum spdlog::level::level_enum log_level_result;
    std::shared_ptr<spdlog::logger> result_logger;
    if (!get_log_config_from_ini(log_level_result,
                                 "logger_cpp_process_udp_result")) {
        result_logger =
            initialize_logger(result_logname, DIR_RESULT_PATH, log_level_result,
                              LOG_FILE_SIZE, LOG_FILE_EXTRA_NUM);
        result_logger->info("UDP Result is running on pid {}", getpid());
    } else {
        throw std::runtime_error(
            "Failed to get a param 'logger_cpp_process_udp_result'");
    }

    // Internal message-queue
    TcpUdpClientQueue client_queue(MSG_QUEUE_SIZE);

    // ping table with timeout
    const std::string ping_table_logname = protocol + "_table_" + ipv4;
    enum spdlog::level::level_enum log_level_ping_table;
    std::shared_ptr<spdlog::logger> ping_table_logger;
    if (!get_log_config_from_ini(log_level_ping_table,
                                 "logger_cpp_process_udp_ping_table")) {
        ping_table_logger = initialize_logger(
            ping_table_logname, DIR_LOG_PATH, log_level_ping_table,
            LOG_FILE_SIZE, LOG_FILE_EXTRA_NUM);
        ping_table_logger->info("UDP ping_table is running on pid {}",
                                getpid());
    } else {
        throw std::runtime_error(
            "Failed to get a param 'logger_cpp_process_udp_ping_table'");
    }

    TcpUdpPinginfoMap ping_table(ping_table_logger, &client_queue,
                                 PINGWEAVE_TABLE_EXPIRY_TIME_UDP_MS);

    // Initialize UDP contexts
    udp_context ctx_tx, ctx_rx;
    if (initialize_contexts(ctx_tx, ctx_rx, ipv4, PINGWEAVE_UDP_PORT_CLIENT,
                            client_logger)) {
        throw std::runtime_error("Failed to initialize UDP contexts.");
    }

    // Start the Result thread
    client_logger->info("Starting UDP result thread (Thread ID: {})...",
                        get_thread_id());
    std::thread result_thread(udp_client_result_thread, ipv4, protocol,
                              &client_queue, result_logger);

    // Start the RX thread
    std::thread rx_thread(udp_client_rx_thread, &ctx_rx, &ping_table,
                          client_logger);

    // Start the TX thread
    std::thread tx_thread(udp_client_tx_thread, &ctx_tx, ipv4, &ping_table,
                          client_logger);

    // termination
    if (result_thread.joinable()) {
        result_thread.join();
    }

    if (tx_thread.joinable()) {
        tx_thread.join();
    }

    if (rx_thread.joinable()) {
        rx_thread.join();
    }
}