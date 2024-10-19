#include "common.hpp"

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
int find_active_port(struct pingweave_context *ctx) {
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

struct ibv_cq *pingweave_cq(struct pingweave_context *ctx) {
    return use_rnic_ts ? ibv_cq_ex_to_cq(ctx->cq_s.cq_ex) : ctx->cq_s.cq;
}

void put_local_info(struct pingweave_dest *my_dest, int is_server,
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

void get_local_info(struct pingweave_dest *rem_dest, int is_server) {
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

int init_ctx(struct pingweave_context *ctx) {
    /* check RNIC timestamping support */
    struct ibv_device_attr_ex attrx;
    if (ibv_query_device_ex(ctx->context, NULL, &attrx)) {
        printf("[WARNING] Couldn't query device for extension features\n");
    } else if (!attrx.completion_timestamp_mask) {
        printf("[WARNING] -> The device isn't completion timestamp capable\n");
    } else {
        printf("Use RNIC Timestamping...\n");
        use_rnic_ts = 1;
        ctx->completion_timestamp_mask = attrx.completion_timestamp_mask;

        // clock metadata
        ctx->ts.comp_recv_max_time_delta = 0;
        ctx->ts.comp_recv_min_time_delta = 0xffffffff;
        ctx->ts.comp_recv_total_time_delta = 0;
        ctx->ts.comp_recv_prev_time = 0;
        ctx->ts.last_comp_with_ts = 0;
        ctx->ts.comp_with_time_iters = 0;
    }

    /* check page size */
    page_size = sysconf(_SC_PAGESIZE);
    printf("Page size: %d\n", page_size);

    ctx->send_flags = IBV_SEND_SIGNALED;

    if (posix_memalign((void **)&ctx->buf, page_size,
                       MESSAGE_SIZE + GRH_SIZE)) {
        std::cerr << "ctx->buf memalign failed\n" << std::endl;
        exit(1);
    }
    memset(ctx->buf, 0x7b, MESSAGE_SIZE + GRH_SIZE);

    {
        int active_port = find_active_port(ctx);
        if (active_port < 0) {
            fprintf(stderr, "Unable to query port info for port %d\n",
                    active_port);
            goto clean_device;
        }
        ctx->active_port = active_port;
    }

    {
        if (USE_EVENT) {
            ctx->channel = ibv_create_comp_channel(ctx->context);
            if (!ctx->channel) {
                fprintf(stderr, "Couldn't create completion channel\n");
                goto clean_device;
            }
        } else {
            ctx->channel = NULL;
        }
    }

    {
        ctx->pd = ibv_alloc_pd(ctx->context);
        if (!ctx->pd) {
            fprintf(stderr, "Couldn't allocate PD\n");
            goto clean_comp_channel;
        }
    }

    {
        ctx->mr = ibv_reg_mr(ctx->pd, ctx->buf, MESSAGE_SIZE + GRH_SIZE,
                             IBV_ACCESS_LOCAL_WRITE);
        if (!ctx->mr) {
            fprintf(stderr, "Couldn't register MR\n");
            goto clean_pd;
        }
    }

    {
        if (use_rnic_ts) {
            struct ibv_cq_init_attr_ex attr_ex = {};
            attr_ex.cqe = RX_DEPTH + 1;
            attr_ex.cq_context = NULL;
            attr_ex.channel = ctx->channel;
            attr_ex.comp_vector = 0;
            attr_ex.wc_flags =
                IBV_WC_EX_WITH_BYTE_LEN | IBV_WC_EX_WITH_COMPLETION_TIMESTAMP;
            ctx->cq_s.cq_ex = ibv_create_cq_ex(ctx->context, &attr_ex);
        } else {
            ctx->cq_s.cq = ibv_create_cq(ctx->context, RX_DEPTH + 1, NULL,
                                         ctx->channel, 0);
        }

        if (!pingweave_cq(ctx)) {
            fprintf(stderr, "Couldn't create CQ\n");
            goto clean_mr;
        }
    }

    {
        struct ibv_qp_attr attr;
        struct ibv_qp_init_attr init_attr = {};
        init_attr.send_cq = pingweave_cq(ctx);
        init_attr.recv_cq = pingweave_cq(ctx);
        init_attr.cap.max_send_wr = 3;
        init_attr.cap.max_recv_wr = RX_DEPTH;
        init_attr.cap.max_send_sge = 3;
        init_attr.cap.max_recv_sge = 3;
        init_attr.qp_type = IBV_QPT_UD;

        ctx->qp = ibv_create_qp(ctx->pd, &init_attr);
        if (!ctx->qp) {
            fprintf(stderr, "Couldn't create QP\n");
            goto clean_cq;
        }

        ibv_query_qp(ctx->qp, &attr, IBV_QP_CAP, &init_attr);
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
            fprintf(stderr, "Failed to modify QP to INIT\n");
            goto clean_qp;
        }
    }

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

int connect_ctx(struct pingweave_context *ctx) {
    struct ibv_qp_attr attr = {};
    attr.qp_state = IBV_QPS_RTR;
    if (ibv_modify_qp(ctx->qp, &attr, IBV_QP_STATE)) {
        fprintf(stderr, "Failed to modify QP to RTR\n");
        return 1;
    }

    attr.qp_state = IBV_QPS_RTS;
    attr.sq_psn = lrand48() & 0xffffff;

    if (ibv_modify_qp(ctx->qp, &attr, IBV_QP_STATE | IBV_QP_SQ_PSN)) {
        fprintf(stderr, "Failed to modify QP to RTS\n");
        return 1;
    }

    return 0;
}

int post_recv(struct pingweave_context *ctx, int n) {
    /* generate a unique wr_id */
    uint64_t randval = static_cast<uint64_t>(lrand48() % UINT8_MAX) * 1000;
    printf("  [POST] RECV with wr_id: %lu\n", randval);
    struct ibv_sge list = {};
    list.addr = (uintptr_t)ctx->buf;
    list.length = MESSAGE_SIZE + GRH_SIZE;
    list.lkey = ctx->mr->lkey;

    struct ibv_recv_wr wr = {};
    wr.wr_id = randval;
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

int post_send(struct pingweave_context *ctx, struct pingweave_dest rem_dest,
              std::string msg) {
    struct ibv_ah_attr ah_attr = {};
    ah_attr.is_global = 0;
    ah_attr.dlid = rem_dest.lid;
    ah_attr.sl = SERVICE_LEVEL;
    ah_attr.src_path_bits = 0;
    ah_attr.port_num = ctx->active_port;

    if (rem_dest.gid.global.interface_id) {
        ah_attr.is_global = 1;
        ah_attr.grh.hop_limit = 1;
        ah_attr.grh.dgid = rem_dest.gid;
        ah_attr.grh.sgid_index = GID_INDEX;
    }

    ctx->ah = ibv_create_ah(ctx->pd, &ah_attr);
    if (!ctx->ah) {
        fprintf(stderr, "Failed to create AH\n");
        return 1;
    }

    /* save a message to buffer */
    strcpy(ctx->buf + GRH_SIZE, msg.c_str());

    /* generate a unique wr_id */
    uint64_t randval = static_cast<uint64_t>(lrand48() % UINT8_MAX);
    printf("  [POST] SEND with wr_id: %lu\n", randval);
    struct ibv_sge list = {};
    list.addr = (uintptr_t)ctx->buf + GRH_SIZE;
    list.length = MESSAGE_SIZE;
    list.lkey = ctx->mr->lkey;

    struct ibv_send_wr wr = {};
    wr.wr_id = randval;
    wr.sg_list = &list;
    wr.num_sge = 1;
    wr.opcode = IBV_WR_SEND;
    wr.send_flags = ctx->send_flags;
    wr.wr.ud.ah = ctx->ah;
    wr.wr.ud.remote_qpn = rem_dest.qpn;
    wr.wr.ud.remote_qkey = 0x11111111;
    struct ibv_send_wr *bad_wr;

    return ibv_post_send(ctx->qp, &wr, &bad_wr);
}

std::string get_source_directory() { return SOURCE_DIR; }

std::shared_ptr<spdlog::logger> init_single_logger(std::string logname) {
    spdlog::drop_all();
    auto logger = spdlog::rotating_logger_mt(
        logname, get_source_directory() + "/../logs/client_rx.log",
        LOG_FILE_SIZE, LOG_FILE_EXTRA_NUM);
    logger->set_pattern(LOG_FORMAT);
    logger->set_level(LOG_LEVEL_PRODUCER);
    logger->flush_on(spdlog::level::debug);
    logger->info("client_rx running (PID: {})", getpid());
    return logger;
}