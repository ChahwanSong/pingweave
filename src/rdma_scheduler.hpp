#pragma once

#include "rdma_common.hpp"
#include "scheduler.hpp"

// vector to store (IP, GID, LID, QPN)
typedef std::vector<std::tuple<std::string, std::string, uint32_t, uint32_t>>
    rdmaAddressInfo_t;
// set to check duplication (IP)
typedef std::unordered_set<std::string> rdmaAddressIp_t;

class RdmaMsgScheduler : public MsgScheduler {
   public:
    RdmaMsgScheduler(const std::string &ip, const std::string &protocol,
                     std::shared_ptr<spdlog::logger> logger)
        : MsgScheduler(ip, protocol, logger) {}
    ~RdmaMsgScheduler() {}

    int next(std::tuple<std::string, std::string, uint32_t, uint32_t> &result,
             uint64_t &time_sleep_us) {
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
                time_sleep_us = 1000000;   // if no addr to send, sleep 1 second
                return PINGWEAVE_FAILURE;  // No address information available
            }
        } else {
            time_sleep_us = inter_ping_interval_us - ping_elapsed_time_us;
            return PINGWEAVE_FAILURE;  // Called too soon
        }
    }

   private:
    rdmaAddressInfo_t addressInfo;

    void load_address_info() {
        // start with clean-slate
        rdmaAddressInfo_t addressInfoNew;
        rdmaAddressIp_t addressIpNew;
        fkyaml::node pinglist;
        fkyaml::node addressStore;
        std::vector<std::string> relevantIps;
        addr_idx = 0;
        bool failed = false;

        // Load pinglist.yaml
        try {
            std::ifstream ifs_pinglist(get_src_dir() + DIR_DOWNLOAD_PATH +
                                       "/pinglist.yaml");
            pinglist = fkyaml::node::deserialize(ifs_pinglist);
        } catch (const std::exception &e) {
            logger->error(
                "Failed to load a pinglist.yaml - fkyaml:deserialize: {}",
                e.what());
            failed = true;
            goto failed;
        }

        // Load address_store.yaml
        try {
            std::ifstream ifs_addressStore(get_src_dir() + DIR_DOWNLOAD_PATH +
                                           "/address_store.yaml");
            addressStore = fkyaml::node::deserialize(ifs_addressStore);
        } catch (const std::exception &e) {
            logger->error(
                "Failed to load a address_store.yaml - fkyaml:deserialize: {}",
                e.what());
            failed = true;
            goto failed;
        }

        // mesh - list of IPs
        try {
            if (pinglist.contains("mesh")) {
                if (pinglist["mesh"].contains(protocol)) {
                    if (pinglist["mesh"][protocol].is_null()) {
                        logger->warn("mesh::{} is null. Skip processing this.",
                                     protocol);
                    } else {
                        for (auto &group : pinglist["mesh"][protocol]) {
                            for (auto &ip : group) {
                                if (ip.get_value_ref<std::string &>() ==
                                    ipaddr) {
                                    for (auto &targetIp : group) {
                                        relevantIps.push_back(
                                            targetIp.get_value_ref<
                                                std::string &>());
                                    }
                                    break;  // move to next group
                                }
                            }
                        }
                    }
                }
            } else {
                logger->debug("No 'mesh' category found in pinglist.yaml");
            }

            // update the RDMA addresses using IPs
            for (auto &it : addressStore) {
                std::string ip = it[0].get_value_ref<std::string &>();
                if (std::find(relevantIps.begin(), relevantIps.end(), ip) !=
                    relevantIps.end()) {
                    // filter dupllication
                    if (addressIpNew.find(ip) == addressIpNew.end()) {
                        // new address
                        addressIpNew.insert(ip);

                        // insert to vector
                        std::string gid = it[1].get_value_ref<std::string &>();
                        uint32_t lid = static_cast<uint32_t>(
                            it[2]
                                .get_value_ref<fkyaml::node::integer_type &>());
                        uint32_t qpn = static_cast<uint32_t>(
                            it[3]
                                .get_value_ref<fkyaml::node::integer_type &>());
                        addressInfoNew.emplace_back(ip, gid, lid, qpn);
                    }
                }
            }

        } catch (const std::exception &e) {
            logger->warn(
                "scheduler: Failed to get IPs from pinglist.yaml::mesh - {}",
                e.what());
            failed = true;
        }

        // clear a temp repo, and sanity check
        relevantIps.clear();
        assert(relevantIps.empty());

        // arrow - list of dstIP if srcIP list includes my node's IP
        try {
            if (pinglist.contains("arrow")) {
                if (pinglist["arrow"].contains(protocol)) {
                    if (pinglist["arrow"][protocol].is_null()) {
                        logger->warn("arrow::{} is null. Skip processing this.",
                                     protocol);
                    } else {
                        for (auto &group : pinglist["arrow"][protocol]) {
                            if (!group.contains("src") ||
                                !group.contains("dst")) {
                                // Both src and dst are required. Skip this
                                // group
                                continue;
                            }

                            // check my IP is in the src IP list
                            bool check_ip_in_src_list = false;
                            for (auto &ip : group["src"]) {
                                std::string srcip =
                                    ip.get_value_ref<std::string &>();

                                if (srcip == ipaddr) {
                                    check_ip_in_src_list = true;
                                    break;  // no need to check for the rest
                                }
                            }

                            // if my IP is not in src IP, skip (no need to send)
                            if (!check_ip_in_src_list) {
                                continue;
                            }

                            // add to my ping list to send
                            for (auto &dstip : group["dst"]) {
                                relevantIps.push_back(
                                    dstip.get_value_ref<std::string &>());
                            }
                        }
                    }
                } else {
                    logger->debug("No 'arrow' category found in pinglist.yaml");
                }
            }

            // update the RDMA addresses using IPs
            for (auto &it : addressStore) {
                std::string ip = it[0].get_value_ref<std::string &>();
                if (std::find(relevantIps.begin(), relevantIps.end(), ip) !=
                    relevantIps.end()) {
                    // filter dupllication
                    if (addressIpNew.find(ip) == addressIpNew.end()) {
                        // new address
                        addressIpNew.insert(ip);

                        // insert to vector
                        std::string gid = it[1].get_value_ref<std::string &>();
                        uint32_t lid = static_cast<uint32_t>(
                            it[2]
                                .get_value_ref<fkyaml::node::integer_type &>());
                        uint32_t qpn = static_cast<uint32_t>(
                            it[3]
                                .get_value_ref<fkyaml::node::integer_type &>());
                        addressInfoNew.emplace_back(ip, gid, lid, qpn);
                    }
                }
            }
        } catch (const std::exception &e) {
            logger->error(
                "scheduler: Failed to get IPs from pinglist::arrow - {}",
                e.what());
            failed = true;
        }

        // logging
        logger->debug("Loaded #{} relevant addresses from pinglist YAML.",
                      addressInfoNew.size());
        if (addressInfo.size() != addressInfoNew.size()) {
            logger->info("AddressInfo changed: {} -> {}", addressInfo.size(),
                         addressInfoNew.size());
        }

        // renew my address info
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

        if (!failed) {
            return;
        }

    failed:
        ++load_yaml_retry_cnt;
        logger->debug("Failed to load YAML (retry {}/{})", load_yaml_retry_cnt,
                      MAX_RETRY_LOAD_YAML);
        if (load_yaml_retry_cnt >= MAX_RETRY_LOAD_YAML) {
            // clear if successively failed >= MAX_RETRY_LOAD_YAML times
            logger->warn(
                "YAML loading failure more than {} times. Clear all cached "
                "address information.",
                MAX_RETRY_LOAD_YAML);
            addressInfo.clear();
            load_yaml_retry_cnt = 0;
        }
    }
};