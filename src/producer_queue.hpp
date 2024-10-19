#pragma once

#include "common.hpp"

struct SharedData {
    char messages[BUFFER_SIZE]
                 [MESSAGE_SIZE];  // Queue of messages with size 64 bytes each
    std::atomic<int> head;  // Head of the queue (consumer reads from here)
    std::atomic<int> tail;  // Tail of the queue (producer writes to here)
    std::atomic<bool> message_ready;  // Flag to indicate when the consumer
                                      // should process messages
};

class ProducerQueue {
   public:
    ProducerQueue(const std::string& shm_name);
    ~ProducerQueue();  // Destructor will handle resource cleanup (RAII)

    // Prevent dynamic class allocation to call deconstructor if runtime error
    void* operator new(size_t) = delete;
    void operator delete(void*) = delete;

    bool sendMessage(const std::string& message);
    void flushBatch();
    std::chrono::time_point<std::chrono::steady_clock> getLastFlushTime();
    std::shared_ptr<spdlog::logger> get_logger();

   private:
    std::shared_ptr<spdlog::logger> logger;
    SharedData* data;  // Pointer to the shared memory
    int shm_fd;
    int messages_in_batch;
    std::chrono::time_point<std::chrono::steady_clock> last_flush_time;
    std::string shm_name;
    uint64_t n_dropped_msgs;

    void initSharedMemory();
    bool writeMessage(const std::string& message);
    uint64_t getNumDroppedMsgs();
};
