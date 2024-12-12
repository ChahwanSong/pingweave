#pragma once

#include "ini.hpp"
#include "logger.hpp"
#include "macro.hpp"

class MsgScheduler {
   protected:
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
                "Failed to load 'report_interval' parameter from "
                "'pingwewave.ini'. Using default: {} microseconds.",
                DEFAULT_INTERVAL_MICROSEC);
            interval_send_ping_microsec = DEFAULT_INTERVAL_MICROSEC;
        }
    }
    virtual ~MsgScheduler() {}

    const int MAX_RETRY_LOAD_YAML = 3;
    const int DEFAULT_INTERVAL_MICROSEC = 1000000;  // default
    uint64_t pingid;
    std::string ipaddr;
    size_t addr_idx;
    std::chrono::steady_clock::time_point last_access_time;
    std::chrono::steady_clock::time_point last_load_time;
    uint64_t inter_ping_interval_us = 1000;  // interval btw each ping
    std::shared_ptr<spdlog::logger> logger;
    int interval_send_ping_microsec = DEFAULT_INTERVAL_MICROSEC;
    int load_yaml_retry_cnt = 0;

    // read yaml files
    virtual void load_address_info() {
        logger->error(
            "Virtual function 'load_address_info' must not be called");
    }
    // schedule for RDMA (dstip, gid, lid, qpn)
    virtual int next(
        std::tuple<std::string, std::string, uint32_t, uint32_t>& result) {
        logger->error("Virtual function 'next' must not be called");
        throw std::runtime_error("virtual function must not be called");
    }
    // schedule for UDP (dstip)
    virtual int next(std::string& result) {
        logger->error("Virtual function 'next' must not be called");
        throw std::runtime_error("virtual function must not be called");
    }
};