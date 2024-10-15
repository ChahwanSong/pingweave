#pragma once

#include <arpa/inet.h>
#include <assert.h>
#include <dirent.h>
#include <endian.h>
#include <getopt.h>
#include <ifaddrs.h>
#include <infiniband/verbs.h>
#include <malloc.h>
#include <netdb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

const static unsigned int msg_size = 50;
const static unsigned int grh_size = 40;
const static unsigned int rx_depth = 50;
const static int gid_index = 0;
const static int service_level = 0;
const static int use_event = 1;
static int use_rnic_ts = 1;
static int page_size = 0;

static std::string msg_from_client = "Ping from client";
static std::string msg_from_server = "Pong from server";

enum {
    PINGPONG_RECV_WRID = 1,
    PINGPONG_SEND_WRID = 2,
};

struct ts_params {
    uint64_t comp_recv_max_time_delta;
    uint64_t comp_recv_min_time_delta;
    uint64_t comp_recv_total_time_delta;
    uint64_t comp_recv_prev_time;
    int last_comp_with_ts;
    unsigned int comp_with_time_iters;
};

struct pingpong_context {
    struct ibv_context *context;
    struct ibv_comp_channel *channel;
    struct ibv_pd *pd;
    struct ibv_mr *mr;
    struct ibv_cq *cq;
    struct ibv_qp *qp;
    struct ibv_ah *ah;
    union {
        struct ibv_cq *cq;
        struct ibv_cq_ex *cq_ex;
    } cq_s;
    char *buf;
    int send_flags;
    int rx_depth;
    int pending;
    int active_port;
    int is_server;
    struct ibv_port_attr portinfo;
    uint64_t completion_timestamp_mask;
    struct ts_params ts;
};

struct pingpong_dest {
    int lid;
    int qpn;
    union ibv_gid gid;
};

enum ibv_mtu pp_mtu_to_enum(int mtu) {
    switch (mtu) {
        case 256:
            return IBV_MTU_256;
        case 512:
            return IBV_MTU_512;
        case 1024:
            return IBV_MTU_1024;
        case 2048:
            return IBV_MTU_2048;
        case 4096:
            return IBV_MTU_4096;
        default:
            return static_cast<ibv_mtu>(0);
    }
}

void wire_gid_to_gid(const char *wgid, union ibv_gid *gid) {
    char tmp[9];
    __be32 v32;
    int i;
    uint32_t tmp_gid[4];
    for (tmp[8] = 0, i = 0; i < 4; ++i) {
        memcpy(tmp, wgid + i * 8, 8);
        sscanf(tmp, "%x", &v32);
        tmp_gid[i] = be32toh(v32);
    }
    memcpy(gid, tmp_gid, sizeof(*gid));
}

void gid_to_wire_gid(const union ibv_gid *gid, char wgid[]) {
    uint32_t tmp_gid[4];
    int i;

    memcpy(tmp_gid, gid, sizeof(tmp_gid));
    for (i = 0; i < 4; ++i) sprintf(&wgid[i * 8], "%08x", htobe32(tmp_gid[i]));
}

// Helper function to find RDMA device by matching network interface
ibv_context *get_context_by_ifname(const char *ifname) {
    char path[512];
    snprintf(path, sizeof(path), "/sys/class/net/%s/device/infiniband", ifname);

    DIR *dir = opendir(path);
    if (!dir) {
        std::cerr << "Unable to open directory: " << path << std::endl;
        return nullptr;
    }

    ibv_device **device_list = ibv_get_device_list(nullptr);
    if (!device_list) {
        std::cerr << "Failed to get RDMA devices list" << std::endl;
        return nullptr;
    }

    struct dirent *entry;
    while ((entry = readdir(dir)) != nullptr) {
        if (entry->d_name[0] != '.') {
            std::string rdma_device_name = entry->d_name;
            closedir(dir);
            for (int i = 0; device_list[i] != nullptr; ++i) {
                if (rdma_device_name == ibv_get_device_name(device_list[i])) {
                    ibv_context *context = ibv_open_device(device_list[i]);
                    ibv_free_device_list(device_list);
                    return context;
                }
            }

            ibv_free_device_list(device_list);
        }
    }

    closedir(dir);
    return nullptr;
}

ibv_context *get_context_by_ip(const char *ip) {
    struct ifaddrs *ifaddr, *ifa;
    int family;
    if (getifaddrs(&ifaddr) == -1) {
        perror("getifaddrs");
        exit(1);
    }

    ibv_context *rdma_context = nullptr;
    for (ifa = ifaddr; ifa != nullptr; ifa = ifa->ifa_next) {
        if (ifa->ifa_addr == nullptr) {
            continue;
        }

        family = ifa->ifa_addr->sa_family;

        if (family == AF_INET) {
            char host[NI_MAXHOST];
            int s = getnameinfo(ifa->ifa_addr, sizeof(struct sockaddr_in), host,
                                NI_MAXHOST, nullptr, 0, NI_NUMERICHOST);
            if (s != 0) {
                std::cerr << "getnameinfo() failed: " << gai_strerror(s)
                          << std::endl;
                exit(1);
            }

            if (strcmp(host, ip) == 0) {
                std::cout << "Found matching interface for IP: " << ip
                          << std::endl;
                rdma_context = get_context_by_ifname(ifa->ifa_name);
                if (rdma_context) {
                    std::cout
                        << "Found RDMA context of interface: " << ifa->ifa_name
                        << std::endl;
                    break;
                } else {
                    std::cerr << "No matching RDMA device found for interface: "
                              << ifa->ifa_name << std::endl;
                }
            }
        }
    }

    freeifaddrs(ifaddr);
    if (!rdma_context) {
        std::cerr << "No matching RDMA device found for IP: " << ip
                  << std::endl;
        exit(1);
    }

    return rdma_context;
}

// RDMA 장치에서 사용 가능한 활성화된 포트 찾기
int find_active_port(struct pingpong_context *ctx) {
    ibv_device_attr device_attr;
    if (ibv_query_device(ctx->context, &device_attr)) {
        std::cerr << "Failed to query device" << std::endl;
        return -1;
    }

    for (int port = 1; port <= device_attr.phys_port_cnt; ++port) {
        if (ibv_query_port(ctx->context, port, &ctx->portinfo)) {
            std::cerr << "Failed to query port " << port << std::endl;
            continue;
        }
        if (ctx->portinfo.state == IBV_PORT_ACTIVE) {
            std::cout << "Found active port: " << port << std::endl;
            return port;
        }
    }

    std::cerr << "No active ports found" << std::endl;
    return -1;
}

void put_local_info(struct pingpong_dest *my_dest, int is_server,
                    std::string ip) {
    std::string filepath = "local_table.csv";
    std::string line, lid, gid, qpn;
    std::vector<std::string> lines;
    char wgid[33];

    lid = std::to_string(my_dest->lid);
    qpn = std::to_string(my_dest->qpn);
    gid_to_wire_gid(&my_dest->gid, wgid);
    gid = std::string(wgid);

    std::ifstream csv_file(filepath);
    while (std::getline(csv_file, line)) {
        lines.push_back(line);
    }
    csv_file.close();

    std::string new_line = ip + "," + lid + "," + qpn + "," + gid;
    int line_num = 2 - is_server;
    if (lines.size() > (size_t)line_num) {
        lines[line_num] = new_line;
    } else {
        lines.push_back(new_line);
    }

    std::ofstream csv_output_file(filepath, std::ios::trunc);
    for (const auto &csv_line : lines) {
        csv_output_file << csv_line << "\n";
    }
    csv_output_file.close();

    printf("Finished writing a line to local_table.csv\n");
}

void get_local_info(struct pingpong_dest *rem_dest, int is_server) {
    std::string line, ip, lid, gid, qpn;

    std::ifstream file("local_table.csv");
    if (!file.is_open()) {
        std::cerr << "Error opening file." << std::endl;
        exit(1);
    }

    std::getline(file, line);

    if (is_server) {
        std::getline(file, line);
    }

    if (std::getline(file, line)) {
        std::istringstream ss(line);

        std::getline(ss, ip, ',');
        std::getline(ss, lid, ',');
        std::getline(ss, qpn, ',');
        std::getline(ss, gid, ',');

        std::cout << "Remote Info: " << ip << ", " << lid << ", " << qpn << ", "
                  << gid << std::endl;
    } else {
        std::cerr << "Error reading second line." << std::endl;
    }

    file.close();

    try {
        rem_dest->lid = std::stoi(lid);
        rem_dest->qpn = std::stoi(qpn);
        wire_gid_to_gid(gid.c_str(), &rem_dest->gid);
    } catch (const std::invalid_argument &e) {
        std::cerr << "Invalid argument: " << e.what() << std::endl;
    } catch (const std::out_of_range &e) {
        std::cerr << "Out of range: " << e.what() << std::endl;
    }
}
