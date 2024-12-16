#pragma once

#include "rdma_common.hpp"
#include "scheduler.hpp"

// Vector to store (IP, GID, LID, QPN)
typedef std::vector<std::tuple<std::string, std::string, uint32_t, uint32_t>>
    rdmaAddressInfo_t;

class RdmaMsgScheduler : public MsgScheduler {
   public:
    RdmaMsgScheduler(const std::string& ip,
                     std::shared_ptr<spdlog::logger> logger)
        : MsgScheduler(ip, logger) {}
    ~RdmaMsgScheduler() {}

    int next(std::tuple<std::string, std::string, uint32_t, uint32_t>& result, uint64_t& time_sleep_us) {
        auto load_now = std::chrono::steady_clock::now();
        auto load_elapsed_time = std::chrono::duration_cast<std::chrono::seconds>(
            load_now - last_load_time);

        // Check the time to load the address_store
        if (load_elapsed_time.count() > LOAD_CONFIG_INTERVAL_SEC) {
            load_address_info();
            last_load_time = load_now;
        }

        auto ping_now = std::chrono::steady_clock::now();
        auto ping_elapsed_time =
            std::chrono::duration_cast<std::chrono::microseconds>(
                ping_now - last_ping_time);
        if (ping_elapsed_time.count() >= inter_ping_interval_us) {
            last_ping_time = ping_now;

            if (!addressInfo.empty()) {
                result = addressInfo[addr_idx % addressInfo.size()];
                addr_idx = (addr_idx + 1) % addressInfo.size();
                time_sleep_us = 0;
                return 1;  // Success
            } else {
                time_sleep_us = 1000000; // if no addr to send, sleep 1 second
                return 0;  // Failure: No address information available
            }
        } else {
            time_sleep_us = inter_ping_interval_us - ping_elapsed_time.count(); //
            return 0;  // Failure: Called too soon
        }
    }

   private:
    rdmaAddressInfo_t addressInfo;
    
    void load_address_info() {
        // start with clean-slate
        rdmaAddressInfo_t addressInfoNew;
        addr_idx = 0;

        try {            
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

            // Load address_store.yaml
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

                    addressInfoNew.emplace_back(ip, gid, lid, qpn);
                }
            }

            logger->debug(
                "Loaded #{} relevant addresses from pinglist YAML.",
                addressInfoNew.size());

            // save the new address info
            addressInfo.clear();
            addressInfo = addressInfoNew;
            load_yaml_retry_cnt = 0;

            if (!addressInfo.empty()) {
                inter_ping_interval_us =
                    interval_send_ping_microsec / addressInfo.size();
            } else {
                // if nothing to send
                inter_ping_interval_us = DEFAULT_INTERVAL_MICROSEC;
            }

            logger->debug("Interval btw ping: {} microseconds",
                          inter_ping_interval_us);
        } catch (const std::exception& e) {
            logger->error(
                "(Retry {}/{}) Failed to load and parse YAML file: {}",
                load_yaml_retry_cnt, MAX_RETRY_LOAD_YAML, e.what());
            ++load_yaml_retry_cnt;
            if (load_yaml_retry_cnt >= MAX_RETRY_LOAD_YAML) {
                // clear if successively failed >= MAX_RETRY_LOAD_YAML times
                addressInfo.clear();
                load_yaml_retry_cnt = 0;
            }
        }
    }
};