#pragma once

#include <infiniband/verbs.h>  // ibv_gid

#include <chrono>
#include <cstdint>
#include <list>
#include <mutex>
#include <shared_mutex>  // for shared_mutex, unique_lock, and shared_lock
#include <unordered_map>

#include "rdma_common.hpp"

// result of ping
struct result_info_t {
    uint64_t pingid;
    uint32_t dstip;

    uint64_t time_ping_send;
    uint64_t client_delay;
    uint64_t network_delay;
    uint64_t server_delay;
    uint32_t success;  // 1: success, 0: failure
};

// fully-blocking SPSC queue
typedef moodycamel::BlockingReaderWriterQueue<struct result_info_t>
    ClientInternalQueue;

enum {
    PINGWEAVE_RECV_PONG = 1,
    PINGWEAVE_RECV_ACK = 1 << 1,
};

struct ping_info_t {
    uint64_t pingid;    // ping ID
    uint32_t qpn;       // destination qpn
    ibv_gid gid;        // destination gid
    std::string dstip;  // destination IP addr

    uint64_t time_ping_send;  // timestamp of PING

    uint64_t client_delay;   // client-side process delay
    uint64_t network_delay;  // pure network-side rtt
    uint64_t server_delay;   // server-side process delay

    int recv_bitmap;  // ACK | PONG | CQE

    // Assignment operator
    ping_info_t& operator=(const ping_info_t& other) {
        if (this == &other) {
            return *this;  // Self-assignment check
        }
        pingid = other.pingid;
        qpn = other.qpn;
        gid = other.gid;
        dstip = other.dstip;

        time_ping_send = other.time_ping_send;
        client_delay = other.client_delay;
        network_delay = other.network_delay;
        server_delay = other.server_delay;

        recv_bitmap = other.recv_bitmap;

        return *this;
    }
};

enum {
    PINGWEAVE_MASK_INIT = 0,
    PINGWEAVE_MASK_RECV_PING_CQE = 1,
    PINGWEAVE_MASK_RECV_PONG = 2,
    PINGWEAVE_MASK_RECV_ACK = 4,
};
#define PINGWEAVE_MASK_RECV                                    \
    (PINGWEAVE_MASK_RECV_PING_CQE | PINGWEAVE_MASK_RECV_PONG | \
     PINGWEAVE_MASK_RECV_ACK)

class PingInfoMap {
   public:
    using Key = uint64_t;
    using TimePoint = std::chrono::steady_clock::time_point;

    explicit PingInfoMap(std::shared_ptr<spdlog::logger> ping_table_logger,
                         ClientInternalQueue* client_queue,
                         int thresholdSeconds = 1)
        : threshold(thresholdSeconds),
          q_ptr(client_queue),
          logger(ping_table_logger) {}

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

    bool update_ping_cqe_time(const Key& key, const uint64_t& x) {
        std::unique_lock lock(mutex_);
        auto it = map.find(key);
        if (it == map.end()) {
            return false;
        }

        // update ping's cqe time
        // if this is called after update_pong_info(), update a value properly
        if (it->second.value.network_delay == 0) {
            it->second.value.network_delay = x;
        } else {
            // CX-6 time mask = 2**63 - 1
            it->second.value.network_delay = calc_time_delta_with_bitwrap(
                x, it->second.value.network_delay, 9223372036854775807LL);
        }

        // update recv bimap
        it->second.value.recv_bitmap |= PINGWEAVE_MASK_RECV_PING_CQE;

        // condition to record
        logging(it->second.value);
        return true;
    }

    bool update_pong_info(const Key& key, const uint64_t& recv_time,
                          const uint64_t mask_recv, const uint64_t& cqe_time,
                          const uint64_t& mask_cqe) {
        std::unique_lock lock(mutex_);
        auto it = map.find(key);
        if (it == map.end()) {
            return false;
        }

        // client delay
        it->second.value.client_delay = calc_time_delta_with_bitwrap(
            it->second.value.client_delay, recv_time, mask_recv);
        // network delay
        if (it->second.value.network_delay != 0) {
            it->second.value.network_delay = calc_time_delta_with_bitwrap(
                it->second.value.network_delay, cqe_time, mask_cqe);
        } else {
            // if PING CQE is somehow delayed and retrieved after PONG
            it->second.value.network_delay = cqe_time;
        }

        // update recv bimap
        it->second.value.recv_bitmap |= PINGWEAVE_MASK_RECV_PONG;

        // condition to record
        logging(it->second.value);
        return true;
    }

    bool update_ack_info(const Key& key, const uint64_t& server_delay) {
        std::unique_lock lock(mutex_);
        auto it = map.find(key);
        if (it == map.end()) {
            return false;
        }

        // update times
        it->second.value.server_delay = server_delay;

        // update recv flag
        it->second.value.recv_bitmap |= PINGWEAVE_MASK_RECV_ACK;

        // condition to record
        logging(it->second.value);
        return true;
    }

    bool logging(const ping_info_t& ping_info) {
        if (ping_info.recv_bitmap == PINGWEAVE_MASK_RECV) {
            // Final result output or storage
            uint64_t client_process_time =
                ping_info.client_delay - ping_info.network_delay;
            uint64_t network_rtt =
                ping_info.network_delay > ping_info.server_delay
                    ? ping_info.network_delay - ping_info.server_delay
                    : ping_info.network_delay;
            uint64_t server_process_time = ping_info.server_delay;

            // logging
            logger->debug("{},{},{},{},{}", ping_info.pingid, ping_info.dstip,
                          client_process_time, network_rtt,
                          server_process_time);

            // send out for analysis
            // ping_time, dstip, ping_time, {each entity's process delays}
            if (!q_ptr->try_enqueue({ping_info.pingid, ip2uint(ping_info.dstip),
                                     ping_info.time_ping_send,
                                     client_process_time, network_rtt,
                                     server_process_time, true})) {
                logger->warn(
                    "Failed to enqueue (pingid {}, success) to result thread",
                    ping_info.pingid);
            }

            if (remove(ping_info.pingid)) {  // if failed to remove
                logger->warn(
                    "Entry for pingid {} does not exist, so cannot remove.",
                    ping_info.pingid);
            }
            return false;  // success
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
        ping_info_t value;
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

            auto& entry = it->second;
            auto elapsedSeconds =
                std::chrono::duration_cast<std::chrono::seconds>(
                    now - entry.timestamp)
                    .count();

            // no more "stale" entries to remove
            if (elapsedSeconds < threshold) {
                return n_remove;
            }

            // remove from map and list
            logger->info("Remove the old entry: pingid {}, dstip: {}",
                         it->second.value.pingid, it->second.value.dstip);

            // failure (condition does not match)
            if (!q_ptr->try_enqueue(
                    {it->second.value.pingid, ip2uint(it->second.value.dstip),
                     it->second.value.time_ping_send, 0, 0, 0, false})) {
                logger->warn(
                    "Failed to enqueue (pingid {}, failed) to result thread",
                    it->second.value.pingid);
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
    const int threshold;
    mutable std::shared_mutex mutex_;  // shared_mutex for read-write locking
    std::shared_ptr<spdlog::logger> logger;
    ClientInternalQueue* q_ptr;
};
