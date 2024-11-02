#include "rdma_scheduler.hpp"

int main() {
    // 테스트할 IP 주소를 초기화
    std::string testIp = "10.200.200.3";

    auto logger = initialize_custom_logger("check", spdlog::level::trace);

    // MsgScheduler 인스턴스 생성
    MsgScheduler scheduler(testIp, "check");

    // 일정 시간 동안 next() 함수를 호출하여 테스트
    std::tuple<std::string, std::string, int> result;
    for (int i = 0; i < 20; ++i) {
        int status = scheduler.next(result);
        if (status == 0) {
            std::cout << "Next Address Info - IP: " << std::get<0>(result)
                      << ", GID: " << std::get<1>(result)
                      << ", QPN: " << std::get<2>(result) << std::endl;
        } else {
            std::cout << "Next call failed or called too soon." << std::endl;
        }
        std::this_thread::sleep_for(std::chrono::seconds(1));  // 1초 대기
    }

    return 0;
}
/**
g++ -std=c++17 -I ../libs -O2 -pthread
-DSOURCE_DIR=\"/home/mason/workspace/pingweave/src\" -L/usr/lib/x86_64-linux-gnu
-o test_scheduler test_scheduler.cpp logger.cpp -I./src -l ibverbs -l yaml-cpp
 */