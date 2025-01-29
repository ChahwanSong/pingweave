#include "producer.hpp"

int main() {
    // ProducerQueue 생성
    ProducerQueue producer("pingweave", "10.200.200.3");

    // 총 전송할 메시지 개수
    const int totalMessages = 10000;

    // 성능 측정용
    auto start = std::chrono::high_resolution_clock::now();

    int droppedCount = 0;

    for (int i = 0; i < totalMessages; ++i) {
        std::string msg = "Hello " + std::to_string(i);
        bool success = producer.writeMessage(msg);
        if (!success) {
            droppedCount++;
        }

        std::this_thread::sleep_for(std::chrono::microseconds(10));
    }

    auto end = std::chrono::high_resolution_clock::now();
    auto elapsedMs =
        std::chrono::duration_cast<std::chrono::milliseconds>(end - start)
            .count();

    std::cout << "Done sending " << totalMessages << " messages.\n";
    std::cout << "Dropped messages: " << droppedCount << "\n";
    std::cout << "Elapsed time: " << elapsedMs << " ms\n";
    return 0;
}