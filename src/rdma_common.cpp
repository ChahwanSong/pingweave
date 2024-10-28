#include "rdma_common.hpp"

// use when writing to file
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

// use when reading from file
void gid_to_wire_gid(const union ibv_gid *gid, char wgid[]) {
    uint32_t tmp_gid[4];
    int i;

    memcpy(tmp_gid, gid, sizeof(tmp_gid));
    for (i = 0; i < 4; ++i) {
        sprintf(&wgid[i * 8], "%08x", htobe32(tmp_gid[i]));
    }
}

std::string parsed_gid(union ibv_gid *gid) {
    char parsed_gid[33];
    inet_ntop(AF_INET6, gid, parsed_gid, sizeof(parsed_gid));
    return std::string(parsed_gid);
}

// calculate time difference with considering bit wrap-around
uint64_t calc_time_delta_with_bitwrap(const uint64_t &t1, const uint64_t &t2,
                                      const uint64_t &mask) {
    uint64_t delta;
    if (t2 >= t1) {  // no wrap around
        delta = t2 - t1;
    } else {  // wrap around
        delta = (mask - t1 + 1) + t2;
    }
    return delta;
}

// Helper function to find RDMA device by matching network interface
int get_context_by_ifname(const char *ifname, struct pingweave_context *ctx) {
    char path[512];
    snprintf(path, sizeof(path), "/sys/class/net/%s/device/infiniband", ifname);

    DIR *dir = opendir(path);
    if (!dir) {
        fprintf(stderr, "Unable to open directory: %s\n", ifname);
        return 1;
    }

    ibv_device **device_list = ibv_get_device_list(nullptr);
    if (!device_list) {
        fprintf(stderr, "Failed to get RDMA devices list: %s\n", ifname);
        return 1;
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
                    ctx->context = context;
                    return 0;
                }
            }

            ibv_free_device_list(device_list);
        }
    }

    closedir(dir);
    return 1;
}

int get_context_by_ip(struct pingweave_context *ctx) {
    struct ifaddrs *ifaddr, *ifa;
    int family;
    if (getifaddrs(&ifaddr) == -1) {
        append_log(ctx->log_msg, "Failed to getifaddrs\n");
        return 1;
    }

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
                append_log(ctx->log_msg, "getnameinfo() %s\n", gai_strerror(s));
                return 1;
            }

            ctx->iface = std::string(ifa->ifa_name);
            if (strcmp(host, ctx->ipv4.c_str()) == 0) {
                if (get_context_by_ifname(ifa->ifa_name, ctx)) {
                    append_log(
                        ctx->log_msg,
                        "No matching RDMA device found for interface  %s\n",
                        ifa->ifa_name);
                    return 1;
                } else {
                    break;
                }
            }
        }
    }

    freeifaddrs(ifaddr);
    if (!ctx->context) {
        append_log(ctx->log_msg, "No matching RDMA device found for IP %s\n",
                   ctx->ipv4);
        return 1;
    }
    return 0;
}

// RDMA 장치에서 사용 가능한 활성화된 포트 찾기
int find_active_port(struct pingweave_context *ctx) {
    ibv_device_attr device_attr;
    if (ibv_query_device(ctx->context, &device_attr)) {
        std::cerr << "Failed to query device" << std::endl;
        return -1;
    }

    for (int port = 1; port <= device_attr.phys_port_cnt; ++port) {
        if (ibv_query_port(ctx->context, port, &ctx->portinfo)) {
            fprintf(stderr, "Failed to query port: %d\n", port);
            continue;
        }
        if (ctx->portinfo.state == IBV_PORT_ACTIVE) {
            // fprintf(stdout, "Found active port: %d\n", port);
            return port;
        }
    }

    fprintf(stderr, "No active ports found.\n");
    return -1;
}

int save_device_info(struct pingweave_context *ctx) {
    const std::string directory = get_source_directory() + "/../local";
    struct stat st = {0};

    if (stat(directory.c_str(), &st) == -1) {
        // create a directory if not exists
        if (mkdir(directory.c_str(), 0744) != 0) {
            append_log(ctx->log_msg, "Cannot create a directory %s\n",
                       directory.c_str());
            return 1;
        }
    }

    // 2. compose a file name
    std::string filename = directory + "/" + ctx->ipv4;

    // 3. save (overwrite)
    std::ofstream outfile(filename);
    if (!outfile.is_open()) {
        append_log(ctx->log_msg, "Cannot open a file %s (%s)\n",
                   filename.c_str(), strerror(errno));
        return 1;
    }

    // save as lines (GID, QPN)
    outfile << ctx->wired_gid << "\n" << ctx->qp->qp_num;  // GID, QPN

    // check error
    if (!outfile) {
        append_log(ctx->log_msg, "Error occued when writing a file %s (%s)\n",
                   filename.c_str(), strerror(errno));
        return 1;
    }

    outfile.close();
    return 0;
}

int load_device_info(union rdma_addr *dst_addr, const std::string &filepath) {
    std::string line, gid, qpn;

    std::ifstream file(filepath);
    if (!file.is_open()) {
        std::cerr << "Error opening file." << std::endl;
        return 1;
    }

    // read gid
    if (std::getline(file, line)) {
        gid = line;
    } else {
        std::cerr << "Error reading first line." << std::endl;
        return 1;
    }

    // read qpn
    if (std::getline(file, line)) {
        qpn = line;
    } else {
        std::cerr << "Error reading second line." << std::endl;
        return 1;
    }

    file.close();

    try {
        dst_addr->x.qpn = std::stoi(qpn);
        wire_gid_to_gid(gid.c_str(), &dst_addr->x.gid);
    } catch (const std::invalid_argument &e) {
        std::cerr << "Invalid argument: " << e.what() << std::endl;
        return 1;
    } catch (const std::out_of_range &e) {
        std::cerr << "Out of range: " << e.what() << std::endl;
        return 1;
    }
    return 0;
}

struct ibv_cq *pingweave_cq(struct pingweave_context *ctx) {
    return ctx->rnic_hw_ts ? ibv_cq_ex_to_cq(ctx->cq_s.cq_ex) : ctx->cq_s.cq;
}

// void put_local_info(struct pingweave_addr *my_dest, int is_server,
//                     std::string ip) {
//     std::string filepath = "local_table.csv";
//     std::string line, lid, gid, qpn;
//     std::vector<std::string> lines;
//     char wgid[33];

//     lid = std::to_string(my_dest->lid);
//     qpn = std::to_string(my_dest->qpn);
//     gid_to_wire_gid(&my_dest->gid, wgid);
//     gid = std::string(wgid);

//     std::ifstream csv_file(filepath);
//     while (std::getline(csv_file, line)) {
//         lines.push_back(line);
//     }
//     csv_file.close();

//     std::string new_line = ip + "," + lid + "," + qpn + "," + gid;
//     int line_num = 2 - is_server;
//     if (lines.size() > (size_t)line_num) {
//         lines[line_num] = new_line;
//     } else {
//         lines.push_back(new_line);
//     }

//     std::ofstream csv_output_file(filepath, std::ios::trunc);
//     for (const auto &csv_line : lines) {
//         csv_output_file << csv_line << "\n";
//     }
//     csv_output_file.close();

//     printf("Finished writing a line to local_table.csv\n");
// }

// void get_local_info(struct pingweave_addr *rem_dest, int is_server) {
//     std::string line, ip, lid, gid, qpn;

//     std::ifstream file("local_table.csv");
//     if (!file.is_open()) {
//         std::cerr << "Error opening file." << std::endl;
//         exit(1);
//     }

//     std::getline(file, line);

//     if (is_server) {
//         std::getline(file, line);
//     }

//     if (std::getline(file, line)) {
//         std::istringstream ss(line);

//         std::getline(ss, ip, ',');
//         std::getline(ss, lid, ',');
//         std::getline(ss, qpn, ',');
//         std::getline(ss, gid, ',');

//         std::cout << "Remote Info: " << ip << ", " << lid << ", " << qpn <<
//         ", "
//                   << gid << std::endl;
//     } else {
//         std::cerr << "Error reading second line." << std::endl;
//     }

//     file.close();

//     try {
//         rem_dest->lid = std::stoi(lid);
//         rem_dest->qpn = std::stoi(qpn);
//         wire_gid_to_gid(gid.c_str(), &rem_dest->gid);
//     } catch (const std::invalid_argument &e) {
//         std::cerr << "Invalid argument: " << e.what() << std::endl;
//     } catch (const std::out_of_range &e) {
//         std::cerr << "Out of range: " << e.what() << std::endl;
//     }
// }

int init_ctx(struct pingweave_context *ctx) {
    // initialize
    ctx->rnic_hw_ts = false;
    ctx->send_flags = IBV_SEND_SIGNALED;

    /* check RNIC timestamping support */
    struct ibv_device_attr_ex attrx;
    if (ibv_query_device_ex(ctx->context, NULL, &attrx)) {
        append_log(ctx->log_msg,
                   "Couldn't query device for extension features.\n");
    } else if (!attrx.completion_timestamp_mask) {
        append_log(ctx->log_msg,
                   "The device isn't completion timestamp capable.\n");
    } else {
        append_log(ctx->log_msg, "RNIC HW timestamping is available.\n");
        ctx->rnic_hw_ts = true;
        ctx->completion_timestamp_mask = attrx.completion_timestamp_mask;
    }

    /* check page size */
    int page_size = sysconf(_SC_PAGESIZE);
    if (posix_memalign((void **)&ctx->buf, page_size,
                       MESSAGE_SIZE + GRH_SIZE)) {
        append_log(ctx->log_msg, "ctx->buf memalign failed.\n");
        return 1;
    }
    memset(ctx->buf, 0x7b, MESSAGE_SIZE + GRH_SIZE);

    {
        int active_port = find_active_port(ctx);
        if (active_port < 0) {
            append_log(ctx->log_msg, "Unable to query port info for port: %d\n",
                       active_port);
            goto clean_device;
        }
        ctx->active_port = active_port;
    }

    {
        ctx->channel = ibv_create_comp_channel(ctx->context);
        if (!ctx->channel) {
            append_log(ctx->log_msg, "Couldn't create completion channel.\n");
            goto clean_device;
        }
    }

    {
        ctx->pd = ibv_alloc_pd(ctx->context);
        if (!ctx->pd) {
            append_log(ctx->log_msg, "Couldn't allocate PD\n");
            goto clean_comp_channel;
        }
    }

    {
        ctx->mr = ibv_reg_mr(ctx->pd, ctx->buf, MESSAGE_SIZE + GRH_SIZE,
                             IBV_ACCESS_LOCAL_WRITE);
        if (!ctx->mr) {
            append_log(ctx->log_msg, "Couldn't register MR\n");
            goto clean_pd;
        }
    }

    {
        if (ctx->rnic_hw_ts) {
            struct ibv_cq_init_attr_ex attr_ex = {};
            attr_ex.cqe = RX_DEPTH + TX_DEPTH;
            attr_ex.cq_context = NULL;
            attr_ex.channel = ctx->channel;
            attr_ex.comp_vector = 0;
            attr_ex.wc_flags =
                IBV_WC_EX_WITH_BYTE_LEN | IBV_WC_EX_WITH_COMPLETION_TIMESTAMP;
            ctx->cq_s.cq_ex = ibv_create_cq_ex(ctx->context, &attr_ex);
        } else {
            ctx->cq_s.cq = ibv_create_cq(ctx->context, RX_DEPTH + TX_DEPTH,
                                         NULL, ctx->channel, 0);
        }

        if (!pingweave_cq(ctx)) {
            append_log(ctx->log_msg, "Couldn't create CQ\n");
            goto clean_mr;
        }
    }

    {
        struct ibv_qp_attr attr = {};
        struct ibv_qp_init_attr init_attr = {};
        init_attr.send_cq = pingweave_cq(ctx);
        init_attr.recv_cq = pingweave_cq(ctx);
        init_attr.cap.max_send_wr = TX_DEPTH;
        init_attr.cap.max_recv_wr = RX_DEPTH;
        init_attr.cap.max_send_sge = 1;
        init_attr.cap.max_recv_sge = 1;
        init_attr.qp_type = IBV_QPT_UD;

        ctx->qp = ibv_create_qp(ctx->pd, &init_attr);
        if (!ctx->qp) {
            append_log(ctx->log_msg, "Couldn't create QP\n");
            goto clean_cq;
        }

        ibv_query_qp(ctx->qp, &attr, IBV_QP_CAP, &init_attr);
        if (init_attr.cap.max_inline_data >= MESSAGE_SIZE + GRH_SIZE) {
            ctx->send_flags |= IBV_SEND_INLINE;
        }
    }

    {
        struct ibv_qp_attr attr = {};
        memset(&attr, 0, sizeof(attr));
        attr.qp_state = IBV_QPS_INIT;
        attr.pkey_index = 0;
        attr.port_num = ctx->active_port;
        attr.qkey = 0x11111111;
        if (ibv_modify_qp(
                ctx->qp, &attr,
                IBV_QP_STATE | IBV_QP_PKEY_INDEX | IBV_QP_PORT | IBV_QP_QKEY)) {
            append_log(ctx->log_msg, "Failed to modify QP to INIT\n");
            goto clean_qp;
        }
    }

    // clear the log message
    ctx->log_msg.clear();

    return 0;

clean_qp:
    ibv_destroy_qp(ctx->qp);

clean_cq:
    ibv_destroy_cq(pingweave_cq(ctx));

clean_mr:
    ibv_dereg_mr(ctx->mr);

clean_pd:
    ibv_dealloc_pd(ctx->pd);

clean_comp_channel:
    if (ctx->channel) ibv_destroy_comp_channel(ctx->channel);

clean_device:
    ibv_close_device(ctx->context);

clean_buffer:
    free(ctx->buf);

    return 1;
}

int prepare_ctx(struct pingweave_context *ctx) {
    struct ibv_qp_attr attr = {};
    attr.qp_state = IBV_QPS_RTR;
    if (ibv_modify_qp(ctx->qp, &attr, IBV_QP_STATE)) {
        append_log(ctx->log_msg, "Failed to modify QP to RTR\n");
        return 1;
    }

    attr.qp_state = IBV_QPS_RTS;
    attr.sq_psn = 0;

    if (ibv_modify_qp(ctx->qp, &attr, IBV_QP_STATE | IBV_QP_SQ_PSN)) {
        append_log(ctx->log_msg, "Failed to modify QP to RTS\n");
        return 1;
    }

    return 0;
}

int make_ctx(struct pingweave_context *ctx, const std::string &ipv4,
             const std::string &logname, const int &is_server,
             const int &is_rx) {
    ctx->log_msg = "";
    ctx->ipv4 = ipv4;
    ctx->is_rx = is_rx;
    if (get_context_by_ip(ctx)) {
        if (check_log(ctx->log_msg)) {
            spdlog::get(logname)->error(ctx->log_msg);
            return 1;
        }
    }

    if (init_ctx(ctx)) {  // Failed to initialize context
        if (check_log(ctx->log_msg)) {
            spdlog::get(logname)->error(ctx->log_msg);
        }
        return 1;
    }

    if (prepare_ctx(ctx)) {  // Failed to prepare context
        if (check_log(ctx->log_msg)) {
            spdlog::get(logname)->error(ctx->log_msg);
        }
        return 1;
    }

    if (ibv_req_notify_cq(pingweave_cq(ctx), 0)) {
        spdlog::get(logname)->error("Couldn't request CQ notification");
        return 1;
    }

    if (ibv_query_gid(ctx->context, ctx->active_port, GID_INDEX, &ctx->gid)) {
        spdlog::get(logname)->error("Could not get my gid for gid index {}",
                                    GID_INDEX);
        return 1;
    }
    // sanity check - always use GID
    assert(ctx->gid.global.subnet_prefix > 0);

    // gid to wired and parsed gid
    gid_to_wire_gid(&ctx->gid, ctx->wired_gid);
    inet_ntop(AF_INET6, &ctx->gid, ctx->parsed_gid, sizeof(ctx->parsed_gid));

    if (is_server && is_rx) {
        // Save Server's RX connection info (ip, qpn, gid) as file
        save_device_info(ctx);
    }

    std::string ctx_node_type = is_server ? "Server" : "Client";
    std::string ctx_send_type = is_rx ? "RX" : "TX";
    spdlog::get(logname)->info(
        "[{} {}] IP: {} has Queue pair with GID: {}, QPN: {}", ctx_node_type,
        ctx_send_type, ipv4, ctx->parsed_gid, ctx->qp->qp_num);
    return 0;
}

int post_recv(struct pingweave_context *ctx, int n, const uint64_t &wr_id) {
    /* generate a unique wr_id */
    struct ibv_sge list = {};
    list.addr = (uintptr_t)ctx->buf;
    list.length = MESSAGE_SIZE + GRH_SIZE;
    list.lkey = ctx->mr->lkey;

    struct ibv_recv_wr wr = {};
    wr.wr_id = wr_id;
    wr.sg_list = &list;
    wr.num_sge = 1;
    struct ibv_recv_wr *bad_wr;
    int cnt = 0;
    for (int i = 0; i < n; ++i) {
        if (ibv_post_recv(ctx->qp, &wr, &bad_wr)) {
            break;
        }
        ++cnt;
    }
    return cnt;
}

int post_send(struct pingweave_context *ctx, union rdma_addr rem_dest,
              const char *msg, const size_t &msg_len, const uint64_t &wr_id) {
    int ret = 0;
    struct ibv_ah_attr ah_attr = {};
    ah_attr.is_global = 0;
    ah_attr.dlid = 0;
    ah_attr.sl = SERVICE_LEVEL;
    ah_attr.src_path_bits = 0;
    ah_attr.port_num = ctx->active_port;

    if (rem_dest.x.gid.global.interface_id) {
        ah_attr.is_global = 1;
        ah_attr.grh.hop_limit = 3;
        ah_attr.grh.dgid = rem_dest.x.gid;
        ah_attr.grh.sgid_index = GID_INDEX;
    } else {
        append_log(ctx->log_msg, "PingWeave does not support LID");
        return 1;
    }

    // sanity check
    ctx->ah = ibv_create_ah(ctx->pd, &ah_attr);
    if (!ctx->ah) {
        append_log(ctx->log_msg, "Failed to create AH\n");
        return 1;
    }
    if (!msg) {
        append_log(ctx->log_msg, "Empty message\n");
        return 1;
    }

    /* save a message to buffer */
    std::memcpy(ctx->buf + GRH_SIZE, msg, msg_len);
    // strncpy(ctx->buf + GRH_SIZE, msg, msg_len);

    /* generate a unique wr_id */
    struct ibv_sge list = {};
    list.addr = (uintptr_t)ctx->buf + GRH_SIZE;
    list.length = MESSAGE_SIZE;
    list.lkey = ctx->mr->lkey;

    struct ibv_send_wr wr = {};
    wr.wr_id = wr_id;
    wr.sg_list = &list;
    wr.num_sge = 1;
    wr.opcode = IBV_WR_SEND;
    wr.send_flags = ctx->send_flags;
    wr.wr.ud.ah = ctx->ah;
    wr.wr.ud.remote_qpn = rem_dest.x.qpn;
    wr.wr.ud.remote_qkey = 0x11111111;
    struct ibv_send_wr *bad_wr;

    ret = ibv_post_send(ctx->qp, &wr, &bad_wr);
    if (ret) {
        append_log(ctx->log_msg, "SEND post is failed\n");
    }
    return ret;
}
