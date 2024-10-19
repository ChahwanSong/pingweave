#include "test_logger_sub.hpp"

int main(int argc, char const *argv[]) {
    auto test_logger = spdlog::rotating_logger_mt(
        "test_logger", "../logs/test.log", 1048576 * 1, 0);

    test();

    return 0;
}

// g++ - std = c++ 17 - I../ libs -O2 test_logger_main.cpp -o test_logger