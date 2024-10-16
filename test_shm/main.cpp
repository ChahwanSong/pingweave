#include <chrono>

#include "producer.hpp"

int main() {
    int total_messages = 100000;  // Total number of messages to send

    Producer producer("/my_shared_memory_queue");

    // For latency measurement
    std::chrono::duration<double, std::micro> total_latency(
        0);  // Total latency accumulator
    auto start_time =
        std::chrono::high_resolution_clock::now();  // Start measuring
                                                    // throughput

    // Send messages one by one with custom messages
    for (int i = 0; i < total_messages; ++i) {
        std::string message =
            "Message " + std::to_string(i + 1);  // Example custom message
        auto message_start = std::chrono::high_resolution_clock::now();
        producer.sendMessage(message);  // Send each message
        auto message_end = std::chrono::high_resolution_clock::now();
        total_latency += message_end - message_start;

        // Check for timeout to flush batch
        auto current_time = std::chrono::steady_clock::now();
        auto elapsed_time =
            std::chrono::duration_cast<std::chrono::milliseconds>(
                current_time - producer.getLastMessageTime())
                .count();
        if (elapsed_time >= BATCH_TIMEOUT_MS) {
            producer.flushBatch();  // Flush the batch if timeout occurs
        }
    }

    // Ensure any remaining messages are flushed at the end
    producer.flushBatch();

    // Measure throughput
    auto end_time = std::chrono::high_resolution_clock::now();
    std::chrono::duration<double> elapsed = end_time - start_time;
    double throughput = total_messages / elapsed.count();

    // Calculate average latency
    double average_latency =
        total_latency.count() / total_messages;  // in microseconds

    std::cout << "C++: Throughput: " << throughput << " messages/second\n";
    std::cout << "C++: Average Latency: " << average_latency << " µs\n";

    return 0;
}