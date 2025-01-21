#pragma once

#include "ini.hpp"
#include "logger.hpp"
#include "macro.hpp"

class MsgScheduler {
   protected:
    MsgScheduler(const std::string& ip, const std::string& protocol,
                 std::shared_ptr<spdlog::logger> logger)
        : ipaddr(ip),
          logger(logger),
          protocol(protocol),
          last_ping_time(get_current_timestamp_steady_clock()),
          last_load_time(get_current_timestamp_steady_clock()),
          addr_idx(0) {
        if (!get_int_param_from_ini(interval_send_ping_microsec,
                                    "interval_send_ping_microsec")) {
            interval_send_ping_microsec = 1000000;
            logger->error(
                "Failed to load 'report_interval' from pingweave.ini. Use {} "
                "microseconds.",
                interval_send_ping_microsec);
        }

        if (!get_int_param_from_ini(load_config_interval_sec,
                                    "interval_sync_pinglist_sec")) {
            load_config_interval_sec = 10;
            logger->error(
                "Failed to load 'interval_sync_pinglist_sec' from "
                "pingweave.ini. Use {} seconds.",
                load_config_interval_sec);
        }
    }
    virtual ~MsgScheduler() {}

    const int MAX_RETRY_LOAD_YAML = 3;
    uint64_t pingid;
    std::string ipaddr;
    std::string protocol;
    size_t addr_idx;
    int load_config_interval_sec;
    std::chrono::steady_clock::time_point last_ping_time;
    std::chrono::steady_clock::time_point last_load_time;
    uint64_t inter_ping_interval_us = 1000;  // interval btw each ping
    std::shared_ptr<spdlog::logger> logger;
    int interval_send_ping_microsec;
    int load_yaml_retry_cnt = 0;

    // read yaml files
    virtual void load_address_info() {
        logger->error(
            "Virtual function 'load_address_info' must not be called");
    }
    // schedule for RDMA (dstip, gid, lid, qpn)
    virtual int next(
        std::tuple<std::string, std::string, uint32_t, uint32_t>& result,
        uint64_t& time_sleep_us) {
        logger->error("Virtual function 'next' must not be called");
        throw std::runtime_error("virtual function must not be called");
    }
    // schedule for UDP (dstip)
    virtual int next(std::string& result, uint64_t& time_sleep_us) {
        logger->error("Virtual function 'next' must not be called");
        throw std::runtime_error("virtual function must not be called");
    }
};