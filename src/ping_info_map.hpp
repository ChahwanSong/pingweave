#pragma once

#include <infiniband/verbs.h>

#include <chrono>
#include <cstdint>
#include <list>
#include <mutex>
#include <shared_mutex>  // for shared_mutex, unique_lock, and shared_lock
#include <unordered_map>

struct ping_info_t {
    uint64_t pingid;  // ping ID
    uint32_t qpn;     // destination qpn
    ibv_gid gid;      // destination gid
    /** TODO: add IP address */
    uint64_t time_ping_send;  // client-delay
    uint64_t time_ping_cqe;   // network-delay
    uint64_t time_server;     // server-delay

    // Assignment operator
    ping_info_t& operator=(const ping_info_t& other) {
        if (this == &other) {
            return *this;  // Self-assignment check
        }
        pingid = other.pingid;
        qpn = other.qpn;
        gid = other.gid;
        time_ping_send = other.time_ping_send;
        time_ping_cqe = other.time_ping_cqe;
        time_server = other.time_server;

        return *this;
    }
};

class PingInfoMap {
   public:
    using Key = uint64_t;
    using TimePoint = std::chrono::steady_clock::time_point;

    explicit PingInfoMap(int thresholdSeconds = 1)
        : threshold(thresholdSeconds) {}

    // if already exists, return false
    bool insert(const Key& key, const ping_info_t& value) {
        std::unique_lock lock(mutex_);
        expireEntries();

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

    // if fail to find, return false
    bool get(const Key& key, ping_info_t& value) {
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

    bool update_time_send(const Key& key, const uint64_t& x) {
        std::unique_lock lock(mutex_);
        auto it = map.find(key);
        if (it == map.end()) {
            return false;
        }

        it->second.value.time_ping_send = x;
        return true;
    }

    bool update_time_cqe(const Key& key, const uint64_t& x) {
        std::unique_lock lock(mutex_);
        auto it = map.find(key);
        if (it == map.end()) {
            return false;
        }

        it->second.value.time_ping_cqe = x;
        return true;
    }

    bool update_time_server(const Key& key, const uint64_t& x) {
        std::unique_lock lock(mutex_);
        auto it = map.find(key);
        if (it == map.end()) {
            return false;
        }

        it->second.value.time_server = x;
        return true;
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
        expireEntries();
        return map.empty();
    }

    size_t size() {
        std::shared_lock lock(mutex_);
        expireEntries();
        return map.size();
    }

   private:
    struct MapEntry {
        ping_info_t value;
        TimePoint timestamp;
        typename std::list<Key>::iterator listIter;
    };

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

    std::unordered_map<Key, MapEntry> map;
    std::list<Key> keyList;
    const int threshold;
    mutable std::shared_mutex mutex_;  // shared_mutex for read-write locking
};
