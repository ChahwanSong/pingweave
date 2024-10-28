#pragma once

#include <chrono>
#include <cstdint>
#include <list>
#include <unordered_map>

template <typename Value>
class TimedMap {
   public:
    using Key = uint64_t;
    using TimePoint = std::chrono::steady_clock::time_point;

    explicit TimedMap(int thresholdSeconds = 1) : threshold(thresholdSeconds) {}

    bool insert(const Key& key, const Value& value) {
        // if already exists, return false
        auto it = map.find(key);
        if (it != map.end()) {
            return false;
        }

        // 리스트 끝에 추가
        auto listIter = keyList.emplace(keyList.end(), key);

        // 맵에 추가
        TimePoint now = std::chrono::steady_clock::now();
        map[key] = {value, now, listIter};
        return true;
    }

    bool get(const Key& key, Value& value) {
        // if fail to find, return false

        expireEntries();

        auto it = map.find(key);
        if (it != map.end()) {
            // found
            value = it->second.value;
            return true;
        }
        // failed
        return false;
    }

    int expireEntries() {
        TimePoint now = std::chrono::steady_clock::now();
        int n_remove = 0;

        while (!keyList.empty()) {
            const Key& key = keyList.front();
            auto it = map.find(key);

            // 맵에 항목이 없으면 리스트에서 제거
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
        return n_remove;
    }

    int remove(const Key& key) {
        auto it = map.find(key);
        if (it != map.end()) {
            keyList.erase(it->second.listIter);
            map.erase(it);
            return true;
        }
        // if nothing to remove
        return false;
    }

    bool empty() {
        // if empty, return true
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
        typename std::list<Key>::iterator listIter;
    };

    std::unordered_map<Key, MapEntry> map;
    std::list<Key> keyList;
    const int threshold;
};