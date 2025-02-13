#pragma once

#include "common.hpp"

struct tcpudp_pinginfo_t {
    uint64_t pingid;          // ping ID
    std::string dstip;        // destination IP addr
    uint64_t time_ping_send;  // timestamp of ping
    uint64_t network_delay;   // ping latency

    // Assignment operator
    tcpudp_pinginfo_t& operator=(const tcpudp_pinginfo_t& other) {
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

class TcpUdpPinginfoMap {
   public:
    using Key = uint64_t;
    using TimePoint = std::chrono::steady_clock::time_point;

    explicit TcpUdpPinginfoMap(
        std::shared_ptr<spdlog::logger> ping_table_logger,
        TcpUdpClientQueue* queue, int threshold_ms = 1000)
        : threshold_ms(threshold_ms),
          client_queue(queue),
          logger(ping_table_logger) {}

    bool insert(const Key& key, const uint64_t& pingid,
                const std::string& dstip) {
        std::unique_lock lock(mutex_);
        expireEntries();

        auto it = map.find(key);
        if (it != map.end()) {
            return PINGWEAVE_FAILURE;
        }

        // Add at the end of list
        auto listIter = keyList.emplace(keyList.end(), key);

        // Add to map
        TimePoint steady_now = get_current_timestamp_steady_clock();
        map[key] = {{pingid, dstip, get_current_timestamp_system_ns(),
                     convert_clock_to_ns(steady_now)},
                    steady_now,
                    listIter};
        return PINGWEAVE_SUCCESS;
    }

    // if fail to find, return false
    bool get(const Key& key, tcpudp_pinginfo_t& value) {
        std::shared_lock lock(mutex_);
        auto it = map.find(key);
        if (it != map.end()) {
            // found
            value = it->second.value;
            return PINGWEAVE_SUCCESS;
        }
        // failed
        return PINGWEAVE_FAILURE;
    }

    bool update_pong_info(const Key& key, const uint64_t& recv_time) {
        std::unique_lock lock(mutex_);
        auto it = map.find(key);
        if (it == map.end()) {
            return PINGWEAVE_FAILURE;
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
                 ping_info.time_ping_send, ping_info.network_delay,
                 PINGWEAVE_RESULT_SUCCESS})) {
            logger->warn(
                "[Queue Full?] pingid {} (-> {}): Failed to enqueue, qlen: {}",
                ping_info.pingid, ping_info.dstip, client_queue->size_approx());
        }

        if (IS_FAILURE(remove(ping_info.pingid))) {  // if failed to remove
            logger->warn("[Expired?] Entry for pingid {} does not exist.",
                         ping_info.pingid);
        }

        return PINGWEAVE_SUCCESS;
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
        tcpudp_pinginfo_t value;
        TimePoint timestamp;
        typename std::list<Key>::iterator listIter;
    };

    // NOTE: this function itself is not thread-safe
    // so, it must be used with unique_lock
    int expireEntries() {
        TimePoint now = get_current_timestamp_steady_clock();
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
                     ping_info.time_ping_send, 0, PINGWEAVE_RESULT_FAILURE})) {
                logger->error(
                    "Failed to enqueue (pingid {}, failed), qlen: {}",
                    ping_info.pingid, client_queue->size_approx());
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
            return PINGWEAVE_SUCCESS;
        }
        // if nothing to remove
        return PINGWEAVE_FAILURE;
    }

    std::unordered_map<Key, MapEntry> map;
    std::list<Key> keyList;
    const int threshold_ms;
    mutable std::shared_mutex mutex_;  // shared_mutex for read-write locking
    std::shared_ptr<spdlog::logger> logger;
    TcpUdpClientQueue* client_queue;
};
