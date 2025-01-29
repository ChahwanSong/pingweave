#pragma once

#include <signal.h>                           // For kill(), signal()
#include <spdlog/async.h>                     // spdlog
#include <spdlog/sinks/rotating_file_sink.h>  // spdlog
#include <spdlog/sinks/stdout_color_sinks.h>  // spdlog
#include <spdlog/spdlog.h>                    // spdlog
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>  // directory
#include <unistd.h>    // close

#include <atomic>  // std::atomic
#include <cerrno>  // errno
#include <chrono>
#include <cstring>  // std::strncpy
#include <future>
#include <iostream>
#include <memory>     // std::shared_ptr
#include <stdexcept>  // std::runtime_error
#include <string>
#include <thread>  // std::this_thread::sleep_for

// ----------------------------------------------------------------------
// Constants & structure definitions
// ----------------------------------------------------------------------

// The size of each message (1024B), and the total number of slots (1024)
const static int MESSAGE_SIZE = 1024;
const static int BUFFER_SIZE = 1024;

// A ring buffer data structure for storing messages
struct SharedData {
    char messages[BUFFER_SIZE][MESSAGE_SIZE];
    std::atomic<int> head;  // The index from which the consumer reads
    std::atomic<int> tail;  // The index where the producer writes
};

// ----------------------------------------------------------------------
// ProducerQueue class
// ----------------------------------------------------------------------
class ProducerQueue {
   public:
    ProducerQueue(const std::string& prefix_name, const std::string& shm_name);
    ~ProducerQueue();  // RAII (releases resources in the destructor)

    // Prevent dynamic memory allocation
    void* operator new(size_t) = delete;
    void operator delete(void*) = delete;

    // Simple ring buffer message-sending
    bool writeMessage(const std::string& message);

    // 1) Immediately discard all remaining messages in the buffer
    void discardAllPendingMessages();

    // 2) Wait until the consumer has read everything
    void waitConsumerToReadAll(int timeoutMs = 3000);

   private:
    // Internal helper functions
    void initSharedMemory();
    uint64_t getNumDroppedMsgs();

    // Member variables
    int shm_fd;
    SharedData* data;  // Pointer to the mapped shared memory
    std::string prefix_name;
    std::string shm_name;
    uint64_t n_dropped_msgs;
    std::shared_ptr<spdlog::logger> logger;
};

// ----------------------------------------------------------------------
// Implementation of ProducerQueue member functions
// (originally in .cpp, now merged into this single header)
// ----------------------------------------------------------------------

// Constructor: Acquire shared memory
ProducerQueue::ProducerQueue(const std::string& prefix_name,
                             const std::string& shm_name)
    : shm_fd(-1),
      data(nullptr),
      prefix_name(prefix_name),
      shm_name(shm_name),
      n_dropped_msgs(0) {
    // Initialize logger
    auto log_path = "./" + prefix_name + "_producer_" + shm_name + ".log";
    logger = spdlog::rotating_logger_mt("producer", "./producer.log",
                                        30 * 1024 * 1024, 1);
    logger->set_pattern("[%Y-%m-%d %H:%M:%S.%f][%l] %v");
    logger->set_level(spdlog::level::info);
    logger->flush_on(spdlog::level::info);
    logger->info("[ProducerQueue] Logger initialized (logname: {}, PID: {})",
                 "producer", getpid());

    // Initialize shared memory
    initSharedMemory();

    logger->info("[ProducerQueue] Initialization done. {}",
                 this->shm_name.c_str());
}

// Initialize and map the shared memory
void ProducerQueue::initSharedMemory() {
    std::string fullName = prefix_name + shm_name;
    shm_fd = shm_open(fullName.c_str(), O_CREAT | O_RDWR, 0666);
    if (shm_fd == -1) {
        logger->error("[ProducerQueue] Failed to create shared memory: {}",
                      fullName);
        throw std::runtime_error("Failed to create shared memory");
    }

    // Set the size of the shared memory object
    if (ftruncate(shm_fd, sizeof(SharedData)) == -1) {
        close(shm_fd);
        shm_unlink(fullName.c_str());
        logger->error("[ProducerQueue] Failed to set size of shared memory: {}",
                      fullName);
        throw std::runtime_error("Failed to set size of shared memory");
    }

    // Map the shared memory into this process
    data = static_cast<SharedData*>(mmap(
        0, sizeof(SharedData), PROT_WRITE | PROT_READ, MAP_SHARED, shm_fd, 0));
    if (data == MAP_FAILED) {
        close(shm_fd);
        shm_unlink(fullName.c_str());
        logger->error("[ProducerQueue] Failed to map shared memory: {}",
                      fullName);
        throw std::runtime_error("Failed to map shared memory");
    }

    // Initialize ring buffer indices
    data->head.store(0, std::memory_order_relaxed);
    data->tail.store(0, std::memory_order_relaxed);

    logger->info("[ProducerQueue] Shared memory initialized: {}", fullName);
}

// Destructor: release shared memory (RAII)
ProducerQueue::~ProducerQueue() {
    // Cleanup buffer data
    discardAllPendingMessages();

    // Cleanup system resources
    if (data) {
        munmap(data, sizeof(SharedData));
        data = nullptr;
    }
    if (shm_fd != -1) {
        close(shm_fd);
        shm_fd = -1;
        shm_unlink(shm_name.c_str());
    }
}

// High-level API for sending a message
bool ProducerQueue::writeMessage(const std::string& message) {
    int headVal = data->head.load(std::memory_order_acquire);
    int tailVal = data->tail.load(std::memory_order_acquire);

    int nextTail = (tailVal + 1) % BUFFER_SIZE;
    if (nextTail == headVal) {
        // Buffer is full
        ++n_dropped_msgs;
        logger->warn("[ProducerQueue] Buffer is full. Message dropped: {}",
                     message);
        return false;
    }

    std::strncpy(data->messages[tailVal], message.c_str(), MESSAGE_SIZE - 1);
    data->messages[tailVal][MESSAGE_SIZE - 1] = '\0';
    data->tail.store(nextTail, std::memory_order_release);
    return true;
}

// 1) Discard all remaining messages in the buffer
void ProducerQueue::discardAllPendingMessages() {
    logger->info("[ProducerQueue] Discard all pending messages.");
    int tailVal = data->tail.load(std::memory_order_acquire);
    data->head.store(tailVal, std::memory_order_release);
}

// 2) Wait for the consumer to read everything
void ProducerQueue::waitConsumerToReadAll(int timeoutMs) {
    auto startTime = std::chrono::steady_clock::now();

    while (true) {
        int headVal = data->head.load(std::memory_order_acquire);
        int tailVal = data->tail.load(std::memory_order_acquire);

        // If buffer is empty
        if (headVal == tailVal) {
            logger->info(
                "[ProducerQueue] Buffer is empty (all read by consumer)");
            break;
        }

        auto now = std::chrono::steady_clock::now();
        auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                             now - startTime)
                             .count();

        if (elapsedMs > timeoutMs) {
            logger->info(
                "[ProducerQueue] Timed out waiting for consumer to read all.");
            break;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}

// get number of dropped messages
uint64_t ProducerQueue::getNumDroppedMsgs() { return n_dropped_msgs; }
