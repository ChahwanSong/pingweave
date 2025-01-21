#pragma once

#include "common.hpp"

struct rdma_pinginfo_t {
    uint64_t pingid;    // ping ID
    uint32_t qpn;       // destination qpn
    ibv_gid gid;        // destination gid
    uint32_t lid;       // destination lid
    std::string dstip;  // destination IP addr

    uint64_t time_ping_send;  // timestamp of PING

    uint64_t client_delay;   // client-side process delay
    uint64_t network_delay;  // pure network-side rtt
    uint64_t server_delay;   // server-side process delay

    uint32_t recv_cnt;  // 3: ping cqe + pong + pong_ack
    int recv_bitmap;    // ACK | PONG | CQE

    // Assignment operator
    rdma_pinginfo_t& operator=(const rdma_pinginfo_t& other) {
        if (this == &other) {
            return *this;  // Self-assignment check
        }
        pingid = other.pingid;
        qpn = other.qpn;
        gid = other.gid;
        lid = other.lid;
        dstip = other.dstip;

        time_ping_send = other.time_ping_send;
        client_delay = other.client_delay;
        network_delay = other.network_delay;
        server_delay = other.server_delay;

        recv_cnt = other.recv_cnt;
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

class RdmaPinginfoMap {
   public:
    using Key = uint64_t;
    using TimePoint = std::chrono::steady_clock::time_point;

    explicit RdmaPinginfoMap(std::shared_ptr<spdlog::logger> ping_table_logger,
                             RdmaClientQueue* queue, int threshold_ms = 1000)
        : threshold_ms(threshold_ms),
          client_queue(queue),
          logger(ping_table_logger) {}

    // if already exists, return false
    bool insert(const Key& key, const rdma_pinginfo_t& value) {
        std::unique_lock lock(mutex_);
        expireEntries();

        auto it = map.find(key);
        if (it != map.end()) {
            return false;
        }

        // Add at the end of list
        auto listIter = keyList.emplace(keyList.end(), key);

        // Add to map
        TimePoint now = get_current_timestamp_steady_clock();
        map[key] = {value, now, listIter};
        return true;
    }

    // if fail to find, return false
    bool get(const Key& key, rdma_pinginfo_t& value) {
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

        if (it->second.value.network_delay == 0) {
            it->second.value.network_delay = x;
        } else {
            /**
             * NOTE: For self-ping, from time to time, At client, CQE of PONG's
             * arrival can be earlier than PING's CQE. This is because self-ping
             * delay can be only 10s of nanoseconds but PING's CQE delay can be
             * longer than that. In this case, we handle the timestamp
             * accordingly.
             */
            it->second.value.network_delay =
                calc_time_delta_with_modulo(x, it->second.value.network_delay,
                                            PINGWEAVE_TIME_CALC_MODULO, logger);
        }

        // update recv bimap and cnt
        it->second.value.recv_bitmap |= PINGWEAVE_MASK_RECV_PING_CQE;
        ++it->second.value.recv_cnt;

        // condition to record
        logging(it->second.value);
        return true;
    }

    bool update_pong_info(const Key& key, const uint64_t& recv_time,
                          const uint64_t& cqe_time) {
        std::unique_lock lock(mutex_);
        auto it = map.find(key);
        if (it == map.end()) {
            // failure
            return false;
        }

        // client delay
        it->second.value.client_delay = calc_time_delta_with_modulo(
            it->second.value.client_delay, recv_time,
            PINGWEAVE_TIME_CALC_MODULO, logger);
        // network delay
        if (it->second.value.network_delay != 0) {
            it->second.value.network_delay = calc_time_delta_with_modulo(
                it->second.value.network_delay, cqe_time,
                PINGWEAVE_TIME_CALC_MODULO, logger);
        } else {
            /**
             * NOTE: As the above case, if CQE of PONG's arrival is earlier
             * than PING's CQE, the network delay variable is initially zero.
             * So, we just over-write to cqe_time. It will be handled
             * accordingly when PING's CQE arrives.
             */
            it->second.value.network_delay = cqe_time;
        }

        // update recv bimap and cnt
        it->second.value.recv_bitmap |= PINGWEAVE_MASK_RECV_PONG;
        ++it->second.value.recv_cnt;

        // condition to record
        logging(it->second.value);

        // success
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

        // update recv bitmap and cnt
        it->second.value.recv_bitmap |= PINGWEAVE_MASK_RECV_ACK;
        ++it->second.value.recv_cnt;

        // condition to record
        logging(it->second.value);
        return true;
    }

    bool logging(const rdma_pinginfo_t& ping_info) {
        if (ping_info.recv_bitmap == PINGWEAVE_MASK_RECV) {
            // sanity check
            if (ping_info.recv_cnt != 3) {
                logger->warn(
                    "[Corrupted] pingid {} (-> {}) recv count must be 3, but "
                    "{}.",
                    ping_info.pingid, ping_info.dstip, ping_info.recv_cnt);
                remove(ping_info.pingid);
                return true;
            }

            // logging
            logger->debug("Pingid:{},DstIP:{},Client:{},Network:{},Server:{}",
                          ping_info.pingid, ping_info.dstip,
                          ping_info.client_delay, ping_info.network_delay,
                          ping_info.server_delay);

            // ping delay of purely client's processing part
            uint64_t client_process_time =
                ping_info.client_delay - ping_info.network_delay;

            uint64_t network_rtt =
                ping_info.network_delay > ping_info.server_delay
                    ? ping_info.network_delay - ping_info.server_delay
                    : ping_info.network_delay;

            // ping delay of purely server's processing part
            uint64_t server_process_time = ping_info.server_delay;

            // ping delay of purely network inflight part
            /**
             * NOTE: If HW timestamp for CQE is not supported,
             * we emulate the HW timestamp by measuring a steady
             * clock. However, because of approximation, the network_delay
             * (PONG's arrival time at client - PING's departure time) can
             * be even smaller than server-side delay.
             * This can happen very rarely, especially for self-ping which
             * has very small network delay.
             * In this case, we just report the network delay but make logging
             * with a warning message.
             */
            // sanity check
            if (ping_info.network_delay <= ping_info.server_delay) {
                logger->warn(
                    "pingid {} - network_delay < server_delay, {}, {} ",
                    ping_info.pingid, ping_info.network_delay,
                    ping_info.server_delay);

                // send out for analysis
                // ping_time, dstip, ping_time, {each entity's process delays}
                if (!client_queue->try_enqueue(
                        {ping_info.pingid, ip2uint(ping_info.dstip),
                         ping_info.time_ping_send, client_process_time,
                         network_rtt, server_process_time,
                         PINGWEAVE_RESULT_WEIRD})) {
                    logger->warn(
                        "[Queue Full?] pingid {} (-> {}): Failed to enqueue to "
                        "result queue",
                        ping_info.pingid, ping_info.dstip);
                }

                if (remove(ping_info.pingid)) {  // if failed to remove
                    logger->warn(
                        "[Expired?] Entry for pingid {} does not exist, so "
                        "cannot remove.",
                        ping_info.pingid);
                }

                return false;
            }

            // send out for analysis
            // ping_time, dstip, ping_time, {each entity's process delays}
            if (!client_queue->try_enqueue(
                    {ping_info.pingid, ip2uint(ping_info.dstip),
                     ping_info.time_ping_send, client_process_time, network_rtt,
                     server_process_time, PINGWEAVE_RESULT_SUCCESS})) {
                logger->warn(
                    "[Queue Full?] pingid {} (-> {}): Failed to enqueue to "
                    "result queue",
                    ping_info.pingid, ping_info.dstip);
            }

            if (remove(ping_info.pingid)) {  // if failed to remove
                logger->warn(
                    "[Expired?] Entry for pingid {} does not exist, so cannot "
                    "remove.",
                    ping_info.pingid);
            }
            return false;
        }

        return true;  // not complete
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
        rdma_pinginfo_t value;
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

            // remove from map and list
            if (ping_info.recv_cnt >= 3) {
                // ignore the case of buffer overlaid
                logger->warn(
                    "[Overlaid?] Pingid {} (-> {}) has recv count {} and "
                    "bitmap "
                    "{}.",
                    ping_info.pingid, ping_info.dstip, ping_info.recv_cnt,
                    ping_info.recv_bitmap);
            } else {
                logger->debug(
                    "[Failed] Pingid {} (-> {}), recv cnt {}, ping_time {}, "
                    "and bitmap {}.",
                    ping_info.pingid, ping_info.dstip, ping_info.recv_cnt,
                    ping_info.time_ping_send, ping_info.recv_bitmap);

                // failure (packets might be lost)
                if (!client_queue->try_enqueue({ping_info.pingid,
                                                ip2uint(ping_info.dstip),
                                                ping_info.time_ping_send, 0, 0,
                                                0, PINGWEAVE_RESULT_FAILURE})) {
                    logger->warn(
                        "[Queue Full?] Failed to enqueue (pingid {}, failed) "
                        "to result "
                        "thread",
                        ping_info.pingid);
                }
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
    RdmaClientQueue* client_queue;
};
