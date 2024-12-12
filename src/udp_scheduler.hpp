#pragma once

#include "scheduler.hpp"

// Vector to store (IP)
typedef std::vector<std::string> udpAddressInfo_t;

class UdpMsgScheduler : public MsgScheduler {
   public:
    UdpMsgScheduler(const std::string& ip,
                     std::shared_ptr<spdlog::logger> logger)
        : MsgScheduler(ip, logger) {}
    ~UdpMsgScheduler() {}

    int next(std::string& result) {
        auto now = std::chrono::steady_clock::now();
        auto loadDuration = std::chrono::duration_cast<std::chrono::seconds>(
            now - last_load_time);

        // Check the time to load the address_store
        if (loadDuration.count() > LOAD_CONFIG_INTERVAL_SEC) {
            load_address_info();
            last_load_time = now;
        }

        auto pingDuration =
            std::chrono::duration_cast<std::chrono::microseconds>(
                now - last_access_time);
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

   private:
    udpAddressInfo_t addressInfo;

    void load_address_info() {
        // start with clean-slate
        udpAddressInfo_t addressInfoNew;
        addr_idx = 0;

        try {
            // Load pinglist.yaml
            std::ifstream ifs_pinglist(get_source_directory() +
                                       DIR_DOWNLOAD_PATH + "/pinglist.yaml");
            fkyaml::node pinglist = fkyaml::node::deserialize(ifs_pinglist);
            
            if (pinglist.contains("udp")) {
                for (auto& group : pinglist["udp"]) {
                    for (auto& ip : group) {
                        if (ip.get_value_ref<std::string&>() == ipaddr) {
                            for (auto& targetIp : group) {
                                addressInfoNew.push_back(
                                    targetIp.get_value_ref<std::string&>());
                            }
                            break;  // move to next group
                        }
                    }
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