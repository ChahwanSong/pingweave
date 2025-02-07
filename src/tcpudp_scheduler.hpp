#pragma once

#include "scheduler.hpp"

// Vector to store (IP)
typedef std::vector<std::string> udpAddressInfo_t;

class TcpUdpMsgScheduler : public MsgScheduler {
   public:
    TcpUdpMsgScheduler(const std::string& ip, const std::string& protocol,
                       std::shared_ptr<spdlog::logger> logger)
        : MsgScheduler(ip, protocol, logger) {}
    ~TcpUdpMsgScheduler() {}

    int next(std::string& result, uint64_t& time_sleep_us) {
        auto load_now = get_current_timestamp_steady_clock();
        auto load_elapsed_time =
            std::chrono::duration_cast<std::chrono::seconds>(load_now -
                                                             last_load_time);

        // Check the time to load the address_store
        if (load_elapsed_time.count() > load_config_interval_sec) {
            load_address_info();
            last_load_time = load_now;
        }

        auto ping_now = get_current_timestamp_steady_clock();
        auto ping_elapsed_time_us =
            std::chrono::duration_cast<std::chrono::microseconds>(
                ping_now - last_ping_time)
                .count();
        if (ping_elapsed_time_us >= inter_ping_interval_us) {
            last_ping_time = ping_now;

            if (!addressInfo.empty()) {
                result = addressInfo[addr_idx % addressInfo.size()];
                addr_idx = (addr_idx + 1) % addressInfo.size();
                time_sleep_us = 0;
                return PINGWEAVE_SUCCESS;  // Success
            } else {
                time_sleep_us = 100000;  // if no addr to send, sleep 0.1 second
                return PINGWEAVE_FAILURE;  // Failure: No address information available
            }
        } else {
            time_sleep_us = inter_ping_interval_us - ping_elapsed_time_us;
            return PINGWEAVE_FAILURE;  // Failure: Called too soon
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
            std::ifstream ifs_pinglist(get_src_dir() + DIR_DOWNLOAD_PATH +
                                       "/pinglist.yaml");
            fkyaml::node pinglist = fkyaml::node::deserialize(ifs_pinglist);

            if (pinglist.contains(protocol)) {
                for (auto& group : pinglist[protocol]) {
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

            logger->debug("Loaded #{} relevant addresses from pinglist YAML.",
                          addressInfoNew.size());
            if (addressInfo.size() != addressInfoNew.size()) {
                logger->info("AddressInfo changed: {} -> {}",
                             addressInfo.size(), addressInfoNew.size());
            }

            // save the new address info
            addressInfo.clear();
            addressInfo = addressInfoNew;
            load_yaml_retry_cnt = 0;

            if (!addressInfo.empty()) {
                inter_ping_interval_us =
                    interval_send_ping_microsec / addressInfo.size();
            } else {
                // if nothing to send
                inter_ping_interval_us = 1000000;
            }

            logger->debug("Interval btw ping: {} microseconds",
                          inter_ping_interval_us);
        } catch (const std::exception& e) {
            ++load_yaml_retry_cnt;
            logger->debug("Failed to load YAML (retry {}/{}). ERROR Msg: {}",
                         load_yaml_retry_cnt, MAX_RETRY_LOAD_YAML, e.what());
            if (load_yaml_retry_cnt >= MAX_RETRY_LOAD_YAML) {
                // clear if successively failed >= MAX_RETRY_LOAD_YAML times
                logger->warn(
                    "Clear address information since YAML loading is failed "
                    "more than 3 times.");
                addressInfo.clear();
                load_yaml_retry_cnt = 0;
            }
        }
    }
};