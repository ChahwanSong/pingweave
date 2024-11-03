#pragma once

#include "rdma_common.hpp"

class MsgScheduler {
   public:
    MsgScheduler(const std::string& ip, const std::string& logname)
        : ipaddr(ip),
          logname(logname),
          last_access_time(std::chrono::steady_clock::now()),
          last_load_time(std::chrono::steady_clock::now()),
          addr_idx(0) {}
    ~MsgScheduler() {}

    int next(std::tuple<std::string, uint32_t, std::string>& result) {
        auto now = std::chrono::steady_clock::now();
        auto duration = std::chrono::duration_cast<std::chrono::microseconds>(
            now - last_access_time);
        auto loadDuration = std::chrono::duration_cast<std::chrono::seconds>(
            now - last_load_time);

        // Check if 10 minutes have passed to call load()
        if (loadDuration.count() >= load_interval_min) {
            load();
            last_load_time = now;
        }

        if (duration.count() >= ping_interval_us) {
            last_access_time = now;

            if (!addressInfo.empty()) {
                result = addressInfo[addr_idx];
                addr_idx = (addr_idx + 1) % addressInfo.size();
                return 1;  // Success
            } else {
                return 0;  // Failure: No address information available
            }
        } else {
            return 0;  // Failure: Called too soon
        }
    }

    void print() const {
        for (const auto& entry : addressInfo) {
            std::string ip = std::get<0>(entry);
            uint32_t qpn = std::get<1>(entry);
            std::string gid = std::get<2>(entry);

            spdlog::get(logname)->info("IP: {}, GID: {}, QPN: {}", ip, gid,
                                       qpn);
        }
    }

   private:
    std::vector<std::tuple<std::string, uint32_t, std::string>>
        addressInfo;  // Vector to store (IP, GID, QPN)
    uint64_t pingid;
    std::string ipaddr;
    size_t addr_idx;
    std::chrono::steady_clock::time_point last_access_time;
    std::chrono::steady_clock::time_point last_load_time;
    uint64_t ping_interval_us = 10;   // 10 microseconds btw each ping
    uint64_t load_interval_min = 10;  // 10 minutes to load yaml
    const uint64_t total_interval_us = 1000000;
    const std::string download_dir = "../download/";
    std::string logname;

    void load() {
        try {
            // clear the storage
            addressInfo.clear();
            addr_idx = 0;

            // Load pinglist.yaml
            YAML::Node pinglist =
                YAML::LoadFile(download_dir + "pinglist.yaml");
            std::vector<std::string> relevantIps;

            if (pinglist["rdma"]) {
                for (const auto& group : pinglist["rdma"]) {
                    for (const auto& ip : group.second) {
                        if (ip.as<std::string>() == ipaddr) {
                            for (const auto& targetIp : group.second) {
                                relevantIps.push_back(
                                    targetIp.as<std::string>());
                            }
                            break;
                        }
                    }
                }
            }

            // Load address_store.yaml
            YAML::Node addressStore =
                YAML::LoadFile(download_dir + "address_store.yaml");
            addressInfo.clear();  // Clear existing data

            for (const auto& it : addressStore) {
                std::string ip = it.first.as<std::string>();
                if (std::find(relevantIps.begin(), relevantIps.end(), ip) !=
                    relevantIps.end()) {
                    std::string gid = it.second[0].as<std::string>();
                    uint32_t qpn = it.second[1].as<uint32_t>();

                    addressInfo.emplace_back(ip, qpn, gid);
                }
            }

            spdlog::get(logname)->info(
                "Loaded {} relevant addresses from YAML.", addressInfo.size());

            if (!addressInfo.empty()) {
                ping_interval_us = total_interval_us / addressInfo.size();
            } else {
                ping_interval_us = 10;
            }

            spdlog::get(logname)->debug("Ping interval: {} microseconds",
                                        ping_interval_us);
        } catch (const YAML::Exception& e) {
            spdlog::get(logname)->error("Failed to load YAML file: {}",
                                        e.what());
            addressInfo.clear();  // If failed, ring becomes empty
        }
    }
};