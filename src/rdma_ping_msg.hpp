#pragma once

#include <infiniband/verbs.h>

#include <chrono>
#include <cstdint>
#include <list>
#include <mutex>
#include <shared_mutex>
#include <unordered_map>

#include "rdma_common.hpp"

class PingMsgMap {
   public:
    using Key = uint64_t;
    using TimePoint = std::chrono::steady_clock::time_point;

    explicit PingMsgMap(int thresholdSeconds = 1)
        : threshold(thresholdSeconds) {}

    // if already exists, return false
    bool insert(const Key& key, const rdma_pingmsg_t& value) {
        std::unique_lock lock(mutex_);
        expireEntries();

        auto it = map.find(key);
        if (it != map.end()) {
            return false;
        }

        // add to list and map with timestamp
        TimePoint now = std::chrono::steady_clock::now();
        auto listIter = keyList.emplace(keyList.end(), key);
        map[key] = {value, now, listIter};
        return true;
    }

    // if fail to find, return false
    bool get(const Key& key, rdma_pingmsg_t& value) {
        std::shared_lock lock(mutex_);
        auto it = map.find(key);
        if (it != map.end()) {
            // found
            value = it->second.value;
            return true;
        }
        // failed
        return false;
    }

    int remove(const Key& key) {
        std::unique_lock lock(mutex_);
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
        std::shared_lock lock(mutex_);
        return map.empty();
    }

    size_t size() {
        std::shared_lock lock(mutex_);
        return map.size();
    }

   private:
    struct MapEntry {
        rdma_pingmsg_t value;
        TimePoint timestamp;
        typename std::list<Key>::iterator listIter;
    };

    int expireEntries() {
        TimePoint now = std::chrono::steady_clock::now();
        int n_remove = 0;

        while (!keyList.empty()) {
            const Key& key = keyList.front();
            auto it = map.find(key);

            // remove from list if map does not have
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

            // remove from map and list
            keyList.pop_front();
            map.erase(it);
            ++n_remove;
        }
        return n_remove;
    }

    std::unordered_map<Key, MapEntry> map;
    std::list<Key> keyList;
    const int threshold;
    mutable std::shared_mutex mutex_;  // shared_mutex for read-write locking
};
