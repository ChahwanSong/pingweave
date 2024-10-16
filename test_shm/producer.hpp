#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstring>
#include <iostream>
#include <thread>

const int QUEUE_SIZE = 1000;      // Queue size
const int MESSAGE_SIZE = 64;      // Message size of 64 bytes
const int BATCH_SIZE = 10;        // Process messages in batches of 10
const int BATCH_TIMEOUT_MS = 10;  // Timeout in milliseconds

struct SharedData {
    char messages[QUEUE_SIZE]
                 [MESSAGE_SIZE];  // Queue of messages with size 64 bytes each
    std::atomic<int> head;  // Head of the queue (consumer reads from here)
    std::atomic<int> tail;  // Tail of the queue (producer writes to here)
    std::atomic<bool> message_ready;  // Flag to indicate when the consumer
                                      // should process messages
};

class Producer {
   public:
    Producer(const std::string& shm_name);
    ~Producer();

    bool sendMessage(const std::string& message);
    void flushBatch();
    void cleanUp();
    std::chrono::time_point<std::chrono::steady_clock> getLastMessageTime()
        const;

   private:
    SharedData* data;
    int shm_fd;
    int sent_messages;
    int messages_in_batch;
    std::string shm_name;

    std::chrono::time_point<std::chrono::steady_clock> last_message_time;

    void initSharedMemory();
    bool writeMessage(const std::string& message, int& next_tail);
};

Producer::Producer(const std::string& shm_name)
    : shm_name(shm_name), sent_messages(0), messages_in_batch(0) {
    initSharedMemory();
    last_message_time =
        std::chrono::steady_clock::now();  // Initialize the last message time
}

void Producer::initSharedMemory() {
    // Create shared memory object
    shm_fd = shm_open(shm_name.c_str(), O_CREAT | O_RDWR, 0666);
    if (shm_fd == -1) {
        std::cerr << "Shared memory creation failed\n";
        throw std::runtime_error("Shared memory creation failed");
    }

    // Set the size of the shared memory object
    if (ftruncate(shm_fd, sizeof(SharedData)) == -1) {
        std::cerr << "Failed to set the size of shared memory\n";
        shm_unlink(shm_name.c_str());  // Clean up in case of failure
        throw std::runtime_error("Failed to set the size of shared memory");
    }

    // Map the shared memory object into the process's address space
    data = static_cast<SharedData*>(mmap(
        0, sizeof(SharedData), PROT_WRITE | PROT_READ, MAP_SHARED, shm_fd, 0));
    if (data == MAP_FAILED) {
        std::cerr << "Mapping failed\n";
        shm_unlink(shm_name.c_str());  // Clean up in case of failure
        throw std::runtime_error("Shared memory mapping failed");
    }

    // Initialize the shared data
    data->head.store(0);
    data->tail.store(0);
    data->message_ready.store(false);
}

bool Producer::writeMessage(const std::string& message, int& next_tail) {
    next_tail = (data->tail.load() + 1) % QUEUE_SIZE;

    // Wait if the queue is full
    if (next_tail == data->head.load()) {
        return false;  // Queue is full
    }

    // Write the message at the tail
    std::strncpy(data->messages[data->tail], message.c_str(), MESSAGE_SIZE - 1);
    data->tail.store(next_tail);

    return true;
}

bool Producer::sendMessage(const std::string& message) {
    int next_tail;

    if (!writeMessage(message, next_tail)) {
        // Wait if the queue is full
        std::this_thread::sleep_for(std::chrono::microseconds(50));
        return false;
    }

    // Increment the internal message counter
    sent_messages++;
    messages_in_batch++;
    last_message_time =
        std::chrono::steady_clock::now();  // Update last message time

    // If the batch size limit is reached, flush the batch
    if (messages_in_batch >= BATCH_SIZE) {
        flushBatch();
    }

    return true;
}

void Producer::flushBatch() {
    if (messages_in_batch > 0) {
        // Set the flag to indicate that the consumer should process the batch
        // of messages
        data->message_ready.store(true);

        // Wait for the consumer to process the batch
        while (data->message_ready.load()) {
            std::this_thread::sleep_for(
                std::chrono::microseconds(50));  // Wait for consumer to process
        }

        // Reset the batch count
        messages_in_batch = 0;
    }
}

void Producer::cleanUp() {
    flushBatch();  // Ensure any remaining messages are flushed before cleaning
                   // up

    // Clean up resources
    munmap(data, sizeof(SharedData));
    close(shm_fd);
    shm_unlink(shm_name.c_str());
}

std::chrono::time_point<std::chrono::steady_clock>
Producer::getLastMessageTime() const {
    return last_message_time;
}

Producer::~Producer() { cleanUp(); }
