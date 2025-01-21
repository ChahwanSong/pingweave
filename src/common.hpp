#pragma once

#include <arpa/inet.h>  // inet_pton, inet_ntop
#include <dirent.h>
#include <fcntl.h>
#include <getopt.h>
#include <ifaddrs.h>
#include <netdb.h>
#include <netinet/in.h>  // sockaddr_in
#include <signal.h>      // For kill(), signal()
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>  // directory
#include <sys/types.h>
#include <sys/wait.h>  // For waitpid()
#include <time.h>
#include <unistd.h>  // For fork(), sleep()

#include <algorithm>
#include <atomic>  // std::atomic
#include <cerrno>  // errno
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>  // strerror
#include <fstream>
#include <functional>
#include <future>
#include <iostream>
#include <list>
#include <memory>
#include <mutex>
#include <numeric>
#include <set>
#include <shared_mutex>  // for shared_mutex, unique_lock, and shared_lock
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <tuple>
#include <unordered_map>
#include <vector>

#include "format.hpp"
#include "ini.hpp"
#include "logger.hpp"
#include "macro.hpp"

extern "C" {
    #include <net/if.h>
}

// fully-blocking SPSC queue
typedef moodycamel::BlockingReaderWriterQueue<struct tcpudp_result_msg_t>
    TcpUdpClientQueue;
typedef moodycamel::BlockingReaderWriterQueue<struct rdma_result_msg_t>
    RdmaClientQueue;
typedef moodycamel::ReaderWriterQueue<union rdma_pingmsg_t> RdmaServerQueue;


std::set<std::string> get_all_local_ips();
int get_my_addr_from_pinglist(const std::string &pinglist_filename,
                              std::set<std::string> &myaddr_roce,
                              std::set<std::string> &myaddr_ib,
                              std::set<std::string> &myaddr_tcp,
                              std::set<std::string> &myaddr_udp);
int get_controller_info_from_ini(std::string &ip, int &port);
int get_int_value_from_ini(IniParser &parser, const std::string &section,
                           const std::string &key);
std::string get_str_value_from_ini(IniParser &parser,
                                   const std::string &section,
                                   const std::string &key);
int get_int_param_from_ini(int &ret, const std::string &key);
int get_str_param_from_ini(std::string &ret, const std::string &key);
int get_log_config_from_ini(enum spdlog::level::level_enum &log_level,
                            const std::string &key);

void delete_files_in_directory(const std::string &directoryPath);
std::string get_thread_id();
uint32_t ip2uint(const std::string &ip);
std::string uint2ip(const uint32_t &ip);
uint64_t make_pingid(const uint32_t &high, const uint32_t &low);
void parse_pingid(const uint64_t &value, uint32_t &high, uint32_t &low);
uint64_t get_current_timestamp_ns();
std::string timestamp_ns_to_string(uint64_t timestamp_ns);
std::string get_current_timestamp_string();
uint64_t get_current_timestamp_steady();
uint64_t calc_time_delta_with_bitwrap(const uint64_t &t1, const uint64_t &t2,
                                      const uint64_t &mask);
uint64_t calc_time_delta_with_modulo(const uint64_t &t1, const uint64_t &t2,
                                     const uint64_t &modulo,
                                     std::shared_ptr<spdlog::logger> logger);
int send_message_to_http_server(const std::string &server_ip, int server_port,
                                const std::string &message,
                                const std::string &api,
                                std::shared_ptr<spdlog::logger> logger);
int message_to_http_server(const std::string &message,
                           const std::string &req_api,
                           std::shared_ptr<spdlog::logger> logger);

// statistics
result_stat_t calc_result_stats(const std::vector<uint64_t> &delays);