#pragma once

#include "udp_common.hpp"

struct udp_pinginfo_t {
    uint64_t pingid;          // ping ID
    std::string dstip;        // destination IP addr
    uint64_t time_ping_send;  // timestamp of ping
    uint64_t network_delay;   // ping latency

    // Assignment operator
    udp_pinginfo_t& operator=(const udp_pinginfo_t& other) {
        if (this == &other) {
            return *this;  // Self-assignment check
        }
        pingid = other.pingid;
        dstip = other.dstip;
        time_ping_send = other.time_ping_send;
        network_delay = other.network_delay;
        return *this;
    }
};

class UdpPinginfoMap {
   public:
    using Key = uint64_t;
    using TimePoint = std::chrono::steady_clock::time_point;

    explicit UdpPinginfoMap(std::shared_ptr<spdlog::logger> ping_table_logger,
                            UdpClientQueue* queue, int threshold_ms = 1000)
        : threshold_ms(threshold_ms),
          client_queue(queue),
          logger(ping_table_logger) {}

    // if already exists, return false
    bool insert(const Key& key, const udp_pinginfo_t& value) {
        std::unique_lock lock(mutex_);
        expireEntries();

        auto it = map.find(key);
        if (it != map.end()) {
            return false;
        }

        // Add at the end of list
        auto listIter = keyList.emplace(keyList.end(), key);

        // Add to map
        TimePoint now = std::chrono::steady_clock::now();
        map[key] = {value, now, listIter};
        return true;
    }

    // if fail to find, return false
    bool get(const Key& key, udp_pinginfo_t& value) {
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

    bool update_pong_info(const Key& key, const uint64_t& recv_time) {
        std::unique_lock lock(mutex_);
        auto it = map.find(key);
        if (it == map.end()) {
            return false;
        }

        auto ping_info = it->second.value;

        // client delay
        ping_info.network_delay = calc_time_delta_with_bitwrap(
            ping_info.network_delay, recv_time, UINT64_MAX);

        // logging
        logger->debug("{},{},{},{}", ping_info.pingid, ping_info.dstip,
                      ping_info.time_ping_send, ping_info.network_delay);

        // send out for analysis
        if (!client_queue->try_enqueue(
                {ping_info.pingid, ip2uint(ping_info.dstip),
                 ping_info.time_ping_send, ping_info.network_delay, true})) {
            logger->warn("pingid {} (-> {}): Failed to enqueue to result queue",
                         ping_info.pingid, ping_info.dstip);
        }

        if (remove(ping_info.pingid)) {  // if failed to remove
            logger->warn(
                "Entry for pingid {} does not exist, so cannot remove.",
                ping_info.pingid);
        }

        return true;
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
        udp_pinginfo_t value;
        TimePoint timestamp;
        typename std::list<Key>::iterator listIter;
    };

    // NOTE: this function itself is not thread-safe
    // so, it must be used with unique_lock
    int expireEntries() {
        TimePoint now = std::chrono::steady_clock::now();
        int n_remove = 0;

        while (!keyList.empty()) {
            const Key& key = keyList.front();
            auto it = map.find(key);

            // if no entry in the map, remove from the list
            if (it == map.end()) {
                keyList.pop_front();
                continue;
            }

            auto elapsed_ms =
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    now - it->second.timestamp)
                    .count();
            auto& ping_info = it->second.value;

            // no more "stale" entries to remove
            if (elapsed_ms < threshold_ms) {
                return n_remove;
            }

            logger->debug(
                "[Failed,Expired] Pingid {} (-> {}), time_send_ping: {}",
                ping_info.pingid, ping_info.dstip,
                timestamp_ns_to_string(ping_info.time_ping_send));

            // failure (packets might be lost)
            if (!client_queue->try_enqueue(
                    {ping_info.pingid, ip2uint(ping_info.dstip),
                     ping_info.time_ping_send, 0, false})) {
                logger->error(
                    "Failed to enqueue (pingid {}, failed) to result thread",
                    ping_info.pingid);
            }

            keyList.pop_front();
            map.erase(it);
            ++n_remove;
        }
        return n_remove;
    }

    // NOTE: this function itself is not thread-safe
    // so, it must be used with unique_lock
    bool remove(const Key& key) {
        auto it = map.find(key);
        if (it != map.end()) {
            keyList.erase(it->second.listIter);
            map.erase(it);
            return false;
        }
        // if nothing to remove
        return true;
    }

    std::unordered_map<Key, MapEntry> map;
    std::list<Key> keyList;
    const int threshold_ms;
    mutable std::shared_mutex mutex_;  // shared_mutex for read-write locking
    std::shared_ptr<spdlog::logger> logger;
    UdpClientQueue* client_queue;
};
