#include <iostream>

#include "timedmap.hpp"

int main() {
    TimedMap timedMap(5);  // 엔트리의 유효 기간을 5초로 설정

    uint32_t key = 42;
    std::vector<std::string> value = {"Hello", "World", "Timed", "Map"};

    timedMap.insert(key, value);

    std::vector<std::string> retrievedValue;
    if (timedMap.get(key, retrievedValue)) {
        std::cout << "키를 찾았습니다: ";
        for (const auto& str : retrievedValue) {
            std::cout << str << " ";
        }
        std::cout << std::endl;
    } else {
        std::cout << "Cannot find a key." << std::endl;
    }

    // 6초 후에 만료 검사
    std::this_thread::sleep_for(std::chrono::seconds(6));
    timedMap.expireEntries();

    if (timedMap.get(key, retrievedValue)) {
        std::cout << "Found a key: ";
        for (const auto& str : retrievedValue) {
            std::cout << str << " ";
        }
        std::cout << std::endl;
    } else {
        std::cout << "Cannot find a key. Entry is expired." << std::endl;
    }

    return 0;
}

// g++ -std=c++11 -pthread -o test test_timedmap.cpp