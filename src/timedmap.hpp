#include <chrono>
#include <cstdint>
#include <list>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

/**
    연산	평균시간 복잡도	최악의 경우 시간복잡도
    삽입	O(1)	O(1)
    제거	O(1)	O(1)
    조회	O(1)	O(n)
 */

class TimedMap {
   public:
    using Key = uint32_t;
    using Value = std::vector<std::string>;
    using TimePoint = std::chrono::steady_clock::time_point;

    explicit TimedMap(int thresholdSeconds = 1) : threshold(thresholdSeconds) {}

    bool insert(const Key& key, const Value& value) {
        // already exists, return false
        auto it = map.find(key);
        if (it != map.end()) {
            // keyList.erase(it->second.listIter);
            // map.erase(it);
            return false;
        }

        // Add to end of list
        auto listIter = keyList.emplace(keyList.end(), key);

        // Add to map
        TimePoint now = std::chrono::steady_clock::now();
        map[key] = {value, now, listIter};
        return true;
    }

    bool get(const Key& key, Value& value) {
        expireEntries();  // 만료된 엔트리 제거

        auto it = map.find(key);
        if (it != map.end()) {
            value = it->second.value;
            return true;
        }
        return false;
    }

    int expireEntries() {
        TimePoint now = std::chrono::steady_clock::now();
        int n_remove = 0;

        while (!keyList.empty()) {
            const Key& key = keyList.front();
            auto it = map.find(key);

            // if no entry in the map
            if (it == map.end()) {
                keyList.pop_front();
                continue;
            }

            auto& entry = it->second;
            auto elapsedSeconds =
                std::chrono::duration_cast<std::chrono::seconds>(
                    now - entry.timestamp)
                    .count();

            if (elapsedSeconds < threshold) {
                return n_remove;
            }
            // 맵과 리스트에서 제거
            keyList.pop_front();
            map.erase(it);
            ++n_remove;
        }
    }

    int remove(const Key& key) {
        auto it = map.find(key);
        if (it != map.end()) {
            keyList.erase(it->second.listIter);
            map.erase(it);
            return true;
        }
        return false;
    }

    bool empty() {
        expireEntries();
        return map.empty();
    }

    size_t size() {
        expireEntries();
        return map.size();
    }

   private:
    struct MapEntry {
        Value value;
        TimePoint timestamp;
        std::list<Key>::iterator listIter;
    };

    std::unordered_map<Key, MapEntry> map;
    std::list<Key> keyList;
    const int threshold;  // 임계값 (초 단위)
};