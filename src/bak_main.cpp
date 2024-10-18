#include <chrono>
#include <functional>
#include <future>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "logger.hpp"

// Exception handling wrapper
template <typename Function>
void run_function(Function func) {
    try {
        func();
    } catch (const std::exception& e) {
        std::cerr << "Thread error: " << e.what() << '\n';
        // Do not shut down; let the monitoring loop handle restarting
    } catch (...) {
        std::cerr << "Unknown thread error" << '\n';
        // Do not shut down; let the monitoring loop handle restarting
    }
}

// Server and client functions with infinite loops
void server_rx() {
    int counter = 0;
    while (true) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        if (++counter == 50) {
            std::cout << "WOW" << std::endl;
            return;
            // throw std::runtime_error("Simulated exception in server_rx");
        }
        // Implement the main logic of server_rx
    }
}

void server_tx() {
    while (true) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        // Implement the main logic of server_tx
    }
}

void client_rx() {
    while (true) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        // Implement the main logic of client_rx
    }
}

void client_tx() {
    while (true) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        // Implement the main logic of client_tx
    }
}

int main() {
    /** Initialize logging objects */
    // producer_queue logger
    std::shared_ptr<spdlog::logger> queue_logger = spdlog::rotating_logger_mt(
        "producer_queue", "../logs/producer_queue.log", size_logfile,
        num_logfile);
    initialize_logger(queue_logger, spdlog::level::debug, spdlog::level::info);

    // server logger
    std::shared_ptr<spdlog::logger> server_logger = spdlog::rotating_logger_mt(
        "server", "../logs/server.log", size_logfile, num_logfile);
    initialize_logger(server_logger, spdlog::level::debug, spdlog::level::info);

    // client logger
    std::shared_ptr<spdlog::logger> client_logger = spdlog::rotating_logger_mt(
        "client", "../logs/client.log", size_logfile, num_logfile);
    initialize_logger(client_logger, spdlog::level::debug, spdlog::level::info);

    // Map to store futures and their corresponding functions
    std::unordered_map<int, std::pair<std::future<void>, std::function<void()>>>
        tasks;

    // Function identifiers
    enum FunctionID { SERVER_RX = 0, SERVER_TX, CLIENT_RX, CLIENT_TX };

    // Initial task creation using std::async
    tasks[SERVER_RX] = {
        std::async(std::launch::async, [=]() { run_function(server_rx); }),
        server_rx};
    tasks[SERVER_TX] = {
        std::async(std::launch::async, [=]() { run_function(server_tx); }),
        server_tx};
    tasks[CLIENT_RX] = {
        std::async(std::launch::async, [=]() { run_function(client_rx); }),
        client_rx};
    tasks[CLIENT_TX] = {
        std::async(std::launch::async, [=]() { run_function(client_tx); }),
        client_tx};

    // Periodically check tasks for completion or exceptions
    while (true) {
        for (auto& task_pair : tasks) {
            int task_id = task_pair.first;
            auto& future = task_pair.second.first;
            auto& func = task_pair.second.second;

            // Non-blocking check if the future is ready
            if (future.wait_for(std::chrono::seconds(0)) ==
                std::future_status::ready) {
                try {
                    future.get();  // Will rethrow exception if one occurred
                    // If the task completed without exception, restart it
                    std::cout << "Task " << task_id
                              << " completed unexpectedly, restarting."
                              << std::endl;
                } catch (const std::exception& e) {
                    // Handle exceptions and restart the task
                    std::cerr << "Task " << task_id << " error: " << e.what()
                              << '\n';
                    std::cout << "Restarting task " << task_id << std::endl;
                } catch (...) {
                    std::cerr << "Task " << task_id << " unknown error\n";
                    std::cout << "Restarting task " << task_id << std::endl;
                }
                // Restart the task
                future = std::async(std::launch::async,
                                    [func]() { run_function(func); });
            }
        }
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }

    // The program is designed to run indefinitely
    // If you need a graceful shutdown mechanism, you can implement it
    // accordingly

    return 0;
}
