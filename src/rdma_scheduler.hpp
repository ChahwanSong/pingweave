#pragma once

#include "rdma_common.hpp"

class MsgScheduler {
   public:
    MsgScheduler(const std::string& ip, std::shared_ptr<spdlog::logger> logger)
        : ipaddr(ip),
          logger(logger),
          last_access_time(std::chrono::steady_clock::now()),
          last_load_time(std::chrono::steady_clock::now()),
          addr_idx(0) {
        int dummy1, dummy2, dummy3;
        if (!get_params_info_from_ini(dummy1, dummy2, dummy3,
                                      interval_send_ping_microsec)) {
            logger->error(
                "Failed to load report_interval parameter from pingwewave.ini. Use default - 1 second.");
            interval_send_ping_microsec = 1000000;
        }
    }
    ~MsgScheduler() {}

    int next(std::tuple<std::string, std::string, uint32_t, uint32_t>& result) {
        auto now = std::chrono::steady_clock::now();
        auto pingDuration =
            std::chrono::duration_cast<std::chrono::microseconds>(
                now - last_access_time);
        auto loadDuration =
            std::chrono::duration_cast<std::chrono::seconds>(
                now - last_load_time);

        // Check the time to load the address_store
        if (loadDuration.count() > LOAD_CONFIG_INTERVAL_SEC) {
            load_address_info();
            last_load_time = now;
        }

        if (pingDuration.count() >= inter_ping_interval_us) {
            last_access_time = now;

            if (!addressInfo.empty()) {
                result = addressInfo[addr_idx % addressInfo.size()];
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
            std::string gid = std::get<1>(entry);
            uint32_t lid = std::get<2>(entry);
            uint32_t qpn = std::get<3>(entry);

            logger->info("IP: {}, GID: {}, LID: {}, QPN: {}", ip, gid, lid,
                         qpn);
        }
    }

   private:
    std::vector<std::tuple<std::string, std::string, uint32_t, uint32_t>>
        addressInfo;  // Vector to store (IP, GID, LID, QPN)
    uint64_t pingid;
    std::string ipaddr;
    size_t addr_idx;
    std::chrono::steady_clock::time_point last_access_time;
    std::chrono::steady_clock::time_point last_load_time;
    uint64_t inter_ping_interval_us = 1000;  // interval btw each ping
    std::shared_ptr<spdlog::logger> logger;
    int interval_send_ping_microsec = 1000000;  // ping interval

    void load_address_info() {
        try {
            // start with clean-slate
            addressInfo.clear();
            addr_idx = 0;

            // Load pinglist.yaml
            std::ifstream ifs_pinglist(get_source_directory() +
                                       DIR_DOWNLOAD_PATH + "/pinglist.yaml");
            fkyaml::node pinglist = fkyaml::node::deserialize(ifs_pinglist);
            std::vector<std::string> relevantIps;

            if (pinglist.contains("rdma")) {
                for (auto& group : pinglist["rdma"]) {
                    for (auto& ip : group) {
                        if (ip.get_value_ref<std::string&>() == ipaddr) {
                            for (auto& targetIp : group) {
                                relevantIps.push_back(
                                    targetIp.get_value_ref<std::string&>());
                            }
                            break;  // move to next group
                        }
                    }
                }
            }

            // Load address_store
            std::ifstream ifs_addressStore(get_source_directory() +
                                           DIR_DOWNLOAD_PATH +
                                           "/address_store.yaml");
            fkyaml::node addressStore =
                fkyaml::node::deserialize(ifs_addressStore);

            for (auto& it : addressStore) {
                std::string ip = it[0].get_value_ref<std::string&>();
                if (std::find(relevantIps.begin(), relevantIps.end(), ip) !=
                    relevantIps.end()) {
                    std::string gid = it[1].get_value_ref<std::string&>();
                    uint32_t lid = static_cast<uint32_t>(
                        it[2].get_value_ref<fkyaml::node::integer_type&>());
                    uint32_t qpn = static_cast<uint32_t>(
                        it[3].get_value_ref<fkyaml::node::integer_type&>());

                    addressInfo.emplace_back(ip, gid, lid, qpn);
                }
            }

            logger->debug(
                "Loaded #{} relevant addresses from address_store YAML.",
                addressInfo.size());

            if (!addressInfo.empty()) {
                inter_ping_interval_us =
                    interval_send_ping_microsec / addressInfo.size();
            } else {
                inter_ping_interval_us = 1000000;  // if nothing to send
            }

            logger->debug("Interval btw ping: {} microseconds",
                          inter_ping_interval_us);
        } catch (const std::exception& e) {
            logger->error("Failed to load and parse YAML file: {}", e.what());
            addressInfo.clear();  // If failed, ring becomes empty
        }
    }
};