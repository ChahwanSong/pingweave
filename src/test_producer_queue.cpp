#include "common.hpp"
#include "producer_queue.hpp"

int main() {
    // 총 전송할 메시지 개수
    const int total_messages = 100000;

    // ProducerQueue 객체 생성 (공유 메모리 초기화)
    ProducerQueue producer_queue("/my_shared_memory_queue");

    // 지연 시간 측정을 위한 변수
    std::chrono::duration<double, std::micro> total_latency(0);  // 총 지연 시간
    auto start_time =
        std::chrono::high_resolution_clock::now();  // 처리량 측정 시작

    // 메시지 하나씩 전송
    for (int i = 0; i < total_messages; ++i) {
        std::string message =
            "Message " + std::to_string(i + 1);  // 예시 메시지

        // 각 메시지 전송 시작 시간 기록
        auto message_start = std::chrono::high_resolution_clock::now();
        if (!producer_queue.sendMessage(message)) {
            spdlog::get("producer")
                ->warn("Message send failed - buffer is full\n");
        }
        auto message_end = std::chrono::high_resolution_clock::now();

        // 각 메시지 전송 후 지연 시간 계산
        total_latency += message_end - message_start;

        // add delay
        usleep(10);

        if (i % 10000 == 0) {
            printf("Message: %s / %d\n", message.c_str(), total_messages);
        }
    }

    // 남은 메시지를 모두 플러시
    producer_queue.flushBatch();

    // 처리량 측정 종료
    auto end_time = std::chrono::high_resolution_clock::now();
    std::chrono::duration<double> elapsed = end_time - start_time;
    double throughput = total_messages / elapsed.count();  // 메시지 처리량 계산

    // 평균 지연 시간 계산
    double average_latency =
        total_latency.count() / total_messages;  // 마이크로초 단위

    // 결과 출력
    std::cout << "C++: Throughput: " << throughput << " messages/second\n";
    std::cout << "C++: Average Latency: " << average_latency << " µs\n";

    return 0;
}
