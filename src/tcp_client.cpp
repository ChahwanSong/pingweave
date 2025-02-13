#include "ipc_producer.hpp"
#include "tcpudp_common.hpp"
#include "tcpudp_ping_info.hpp"
#include "tcpudp_scheduler.hpp"

void tcp_client_tx_thread(const std::string& ipv4,
                          TcpUdpPinginfoMap* ping_table,
                          std::shared_ptr<spdlog::logger> logger,
                          std::atomic<bool>* thread_alive) {
    thread_alive->store(true);
    logger->info("Running a client thread (Thread ID: {})...", get_thread_id());

    uint32_t ping_uid = 0;
    uint64_t time_sleep_us = 0;
    TcpUdpMsgScheduler scheduler(ipv4, "tcp", logger);
    std::string dst_addr;

    try {
        while (true) {
            // Retrieve the next destination for sending
            if (IS_SUCCESS(scheduler.next(dst_addr, time_sleep_us))) {
                // Create a pingid
                auto pingid = make_pingid(ip2uint(ipv4), ping_uid++);

                // thread and detach
                std::thread t(send_tcp_message, ping_table, ipv4, dst_addr,
                              PINGWEAVE_TCP_PORT_SERVER, pingid, logger);
                t.detach();
            } else {
                // sleep until next ping schedule
                std::this_thread::sleep_for(
                    std::chrono::microseconds(time_sleep_us));
            }
        }
    } catch (const std::exception& e) {
        logger->error("Exception in TX thread: {}", e.what());
        thread_alive->store(false);
        throw;  // Propagate exception
    }
}

void tcp_client_result_thread(const std::string& ipv4,
                              TcpUdpClientQueue* client_queue,
                              std::shared_ptr<spdlog::logger> logger,
                              std::atomic<bool>* thread_alive) {
    thread_alive->store(true);
    logger->info("Running Result thread (Thread ID: {})", get_thread_id());

    int report_interval_ms = 10000;
    if (IS_FAILURE(get_int_param_from_ini(
            report_interval_ms, "interval_report_ping_result_millisec"))) {
        logger->error(
            "Failed to load 'report_interval' from pingwewave.ini. Use default "
            "- 10 seconds");
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
    if (IS_FAILURE(
            get_controller_info_from_ini(controller_host, controller_port))) {
        logger->error(
            "Exit the result thread - failed to load pingweave.ini file");
        thread_alive->store(false);
        throw;  // Propagate exception
    }

    // IPC - producer queue
    ProducerQueue ipc_producer("tcp", ipv4);

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
                    logger->warn("Unknown type of result - {}, dstip: {}",
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
        thread_alive->store(false);
        throw;
    }
}

void tcp_client(const std::string& ipv4) {
    // Start the RX thread
    const std::string client_logname = "tcp_client_" + ipv4;
    enum spdlog::level::level_enum log_level_client;
    std::shared_ptr<spdlog::logger> client_logger;
    if (IS_FAILURE(get_log_config_from_ini(log_level_client,
                                           "logger_cpp_process_tcp_client"))) {
        throw std::runtime_error(
            "Failed to get a param 'logger_cpp_process_tcp_client'");
    } else {
        client_logger =
            initialize_logger(client_logname, DIR_LOG_PATH, log_level_client,
                              LOG_FILE_SIZE, LOG_FILE_EXTRA_NUM);
        client_logger->info("TCP Client is running on pid {}", getpid());
    }

    // Inter-thread queue
    const std::string result_logname = "tcp_" + ipv4;
    enum spdlog::level::level_enum log_level_result;
    std::shared_ptr<spdlog::logger> result_logger;
    if (IS_FAILURE(get_log_config_from_ini(log_level_result,
                                           "logger_cpp_process_tcp_result"))) {
        throw std::runtime_error(
            "Failed to get a param 'logger_cpp_process_tcp_result'");
    } else {
        result_logger =
            initialize_logger(result_logname, DIR_RESULT_PATH, log_level_result,
                              LOG_FILE_SIZE, LOG_FILE_EXTRA_NUM);
        result_logger->info("TCP Result is running on pid {}", getpid());
    }

    // Internal message-queue
    TcpUdpClientQueue client_queue(MSG_QUEUE_SIZE);

    // ping table with timeout
    const std::string ping_table_logname = "tcp_table_" + ipv4;
    enum spdlog::level::level_enum log_level_ping_table;
    std::shared_ptr<spdlog::logger> ping_table_logger;
    if (IS_FAILURE(get_log_config_from_ini(
            log_level_ping_table, "logger_cpp_process_tcp_ping_table"))) {
        throw std::runtime_error(
            "Failed to get a param 'logger_cpp_process_tcp_ping_table'");
    } else {
        ping_table_logger = initialize_logger(
            ping_table_logname, DIR_LOG_PATH, log_level_ping_table,
            LOG_FILE_SIZE, LOG_FILE_EXTRA_NUM);
        ping_table_logger->info("TCP ping_table is running on pid {}",
                                getpid());
    }

    TcpUdpPinginfoMap ping_table(ping_table_logger, &client_queue,
                                 PINGWEAVE_TABLE_EXPIRY_TIME_TCP_MS);

    // true: running, false: terminated
    std::atomic<bool> tx_alive{true};
    std::atomic<bool> result_alive{true};

    // Start the Result thread
    client_logger->info("Starting TCP result thread (Thread ID: {})...",
                        get_thread_id());
    std::thread result_thread(tcp_client_result_thread, ipv4, &client_queue,
                              result_logger, &result_alive);

    // Start the client thread
    std::thread tx_thread(tcp_client_tx_thread, ipv4, &ping_table,
                              client_logger, &tx_alive);

    
    while (true) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        if (!tx_alive.load()) {
            message_to_http_server("Unexpected termination: tcp_client_tx_thread", "/alarm", client_logger);
            if (tx_thread.joinable()) {
                tx_thread.join();
            }
            message_to_http_server("Restarted: tcp_client_tx_thread", "/alarm", client_logger);
            tx_alive.store(true);
            client_logger->warn("TX thread terminated unexpectedly. Restarting.");
            tx_thread = std::thread(tcp_client_tx_thread, ipv4, &ping_table, client_logger, &tx_alive);
        }
        if (!result_alive.load()) {
            message_to_http_server("Unexpected termination: tcp_client_result_thread", "/alarm", client_logger);
            if (result_thread.joinable()) {
                result_thread.join();
            }
            message_to_http_server("Restarted: tcp_client_result_thread", "/alarm", client_logger);
            result_alive.store(true);
            client_logger->warn("Result thread terminated unexpectedly. Restarting.");
            result_thread = std::thread(tcp_client_result_thread, ipv4, &client_queue, result_logger, &result_alive);
        }
    }
}

void print_help() {
    std::cout << "Usage: tcp_client <IPv4 address>\n"
              << "Arguments:\n"
              << "  IPv4 address   The target IPv4 address for TCP client.\n"
              << "Options:\n"
              << "  -h, --help     Show this help message.\n";
}

int main(int argc, char* argv[]) {
    if (argc != 2) {
        spdlog::error("Error: Invalid arguments.");
        print_help();
        return 1;
    }

    if ((std::string(argv[1]) == "-h" || std::string(argv[1]) == "--help")) {
        print_help();
        return 0;
    }

    std::string ipv4 = argv[1];

    try {
        tcp_client(ipv4);
    } catch (const std::exception& e) {
        spdlog::error("Exception occurred: {}", e.what());
        return 1;
    }

    return 0;
}