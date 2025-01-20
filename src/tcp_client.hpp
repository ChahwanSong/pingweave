#pragma once


void tcp_client(const std::string& ipv4, const std::string& protocol) {
    // Start the RX thread
    const std::string client_logname = protocol + "_client_" + ipv4;
    enum spdlog::level::level_enum log_level_client;
    std::shared_ptr<spdlog::logger> client_logger;
    if (get_log_config_from_ini(log_level_client,
                                "logger_cpp_process_tcp_client")) {
        client_logger =
            initialize_logger(client_logname, DIR_LOG_PATH, log_level_client,
                              LOG_FILE_SIZE, LOG_FILE_EXTRA_NUM);
        client_logger->info("UDP Client is running on pid {}", getpid());
    } else {
        throw std::runtime_error(
            "Failed to get a param 'logger_cpp_process_tcp_client'");
    }

    // Inter-thread queue
    const std::string result_logname = protocol + "_" + ipv4;
    enum spdlog::level::level_enum log_level_result;
    std::shared_ptr<spdlog::logger> result_logger;
    if (get_log_config_from_ini(log_level_result,
                                "logger_cpp_process_tcp_result")) {
        result_logger =
            initialize_logger(result_logname, DIR_RESULT_PATH, log_level_result,
                              LOG_FILE_SIZE, LOG_FILE_EXTRA_NUM);
        result_logger->info("UDP Result is running on pid {}", getpid());
    } else {
        throw std::runtime_error(
            "Failed to get a param 'logger_cpp_process_tcp_result'");
    }

    // Internal message-queue
    TcpUdpClientQueue client_queue(MSG_QUEUE_SIZE);

    // ping table with timeout
    const std::string ping_table_logname = protocol + "_table_" + ipv4;
    enum spdlog::level::level_enum log_level_ping_table;
    std::shared_ptr<spdlog::logger> ping_table_logger;
    if (get_log_config_from_ini(log_level_ping_table,
                                "logger_cpp_process_tcp_ping_table")) {
        ping_table_logger = initialize_logger(
            ping_table_logname, DIR_LOG_PATH, log_level_ping_table,
            LOG_FILE_SIZE, LOG_FILE_EXTRA_NUM);
        ping_table_logger->info("UDP ping_table is running on pid {}",
                                getpid());
    } else {
        throw std::runtime_error(
            "Failed to get a param 'logger_cpp_process_tcp_ping_table'");
    }

    TcpUdpPinginfoMap ping_table(ping_table_logger, &client_queue,
                                 PINGWEAVE_TABLE_EXPIRY_TIME_UDP_MS);

    // Initialize UDP contexts
    tcp_context ctx_tx, ctx_rx;
    if (initialize_contexts(ctx_tx, ctx_rx, ipv4, client_logger)) {
        throw std::runtime_error("Failed to initialize UDP contexts.");
    }

    // Start the Result thread
    client_logger->info("Starting UDP result thread (Thread ID: {})...",
                        get_thread_id());
    std::thread result_thread(tcp_client_result_thread, ipv4, protocol,
                              &client_queue, result_logger);

    // Start the RX thread
    std::thread rx_thread(tcp_client_rx_thread, &ctx_rx, ipv4, &ping_table,
                          client_logger);

    // Start the TX thread
    std::thread tx_thread(tcp_client_tx_thread, &ctx_tx, ipv4, &ping_table,
                          client_logger);

    // termination
    if (tx_thread.joinable()) {
        tx_thread.join();
    }

    if (result_thread.joinable()) {
        result_thread.join();
    }
}