#include <spdlog/async.h>
#include <spdlog/sinks/rotating_file_sink.h>
#include <spdlog/spdlog.h>

void test() {
    int n = 0;
    while (n < 100000) {
        spdlog::get("test_logger")->info("This is a test logging - {}.", ++n);
        spdlog::get("test_logger")->warn("This is a test logging - {}", ++n);
        spdlog::get("test_logger")->error("This is a test logging - {}", ++n);
    }
}