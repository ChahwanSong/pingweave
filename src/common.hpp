#pragma once

#include <arpa/inet.h>
#include <dirent.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <netdb.h>
#include <netinet/in.h>
#include <signal.h>  // For kill(), signal()
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>  // directory
#include <sys/stat.h>
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
#include <future>
#include <iostream>
#include <list>
#include <mutex>
#include <numeric>
#include <set>
#include <shared_mutex>  // for shared_mutex, unique_lock, and shared_lock
#include <sstream>
#include <string>
#include <thread>
#include <tuple>
#include <unordered_map>
#include <vector>

#include "format.hpp"
#include "ini.hpp"
#include "logger.hpp"
#include "macro.hpp"

std::set<std::string> get_all_local_ips();
int get_my_addr_from_pinglist(const std::string &pinglist_filename,
                              std::set<std::string> &myaddr_rdma,
                              std::set<std::string> &myaddr_udp);
int get_controller_info_from_ini(std::string &ip, int &port);
int get_params_info_from_ini(int &val_1, int &val_2, int &val_3, int &val_4);
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