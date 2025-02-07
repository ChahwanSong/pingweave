#include "rdma_common.hpp"

// Helper function to find RDMA device by matching network interface
int get_context_by_ifname(const char *ifname, struct rdma_context *ctx) {
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

int get_context_by_ip(struct rdma_context *ctx,
                      std::shared_ptr<spdlog::logger> logger) {
    struct ifaddrs *ifaddr, *ifa;
    int family;
    if (getifaddrs(&ifaddr) == -1) {
        logger->error("Failed to getifaddrs");
        return PINGWEAVE_FAILURE;
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
                logger->error("getnameinfo(): {}", gai_strerror(s));
                return PINGWEAVE_FAILURE;
            }

            ctx->iface = std::string(ifa->ifa_name);
            if (strcmp(host, ctx->ipv4.c_str()) == 0) {
                if (get_context_by_ifname(ifa->ifa_name, ctx)) {
                    logger->error(
                        "No matching RDMA device found for interface: {}",
                        ifa->ifa_name);
                    freeifaddrs(ifaddr);
                    return PINGWEAVE_FAILURE;
                } else {
                    break;
                }
            }
        }
    }

    freeifaddrs(ifaddr);
    if (!ctx->context) {
        logger->error("No matching RDMA device found for IP {}", ctx->ipv4);
        return PINGWEAVE_FAILURE;
    }

    // success
    return PINGWEAVE_SUCCESS;
}

// Find a valid active port on RDMA devices
int find_active_port(struct rdma_context *ctx,
                     std::shared_ptr<spdlog::logger> logger) {
    ibv_device_attr device_attr;
    if (ibv_query_device(ctx->context, &device_attr)) {
        logger->error("Failed to query device");
        return -1;
    }

    for (int port = 1; port <= device_attr.phys_port_cnt; ++port) {
        if (ibv_query_port(ctx->context, port, &ctx->portinfo)) {
            logger->error("Failed to query port {}", port);
            continue;
        }
        if (ctx->portinfo.state == IBV_PORT_ACTIVE) {
            logger->debug("Found active port: {}", port);
            return port;
        }
    }

    logger->error("No active ports found.");
    return -1;
}

// Get GID table index
int get_gid_table_index(struct rdma_context *ctx,
                        std::shared_ptr<spdlog::logger> logger) {
    struct ibv_port_attr port_attr;

    if (ibv_query_port(ctx->context, ctx->active_port, &port_attr)) {
        logger->error("Failed to query port attributes.");
        return -1;
    }

    int gid_table_size = port_attr.gid_tbl_len;
    if (gid_table_size <= 0) {
        logger->error("No GIDs available.");
        return -1;
    }

    union ibv_gid last_gid;
    int gid_index = -1;            // last valid  GID index
    int preferred_gid_index = -1;  // GID index starting with "::ffff:"

    for (int i = gid_table_size - 1; i >= 0; --i) {
        if (ibv_query_gid(ctx->context, ctx->active_port, i, &last_gid) == 0) {
            auto gid_str = parsed_gid(&last_gid);
            if (!gid_str.empty() && gid_str != "::") {
                logger->debug("Device's GID [{}]: {}", i, gid_str);

                // Prioritize GID index starting with "::ffff:"
                if (gid_str.find("::ffff:") == 0 && preferred_gid_index == -1) {
                    preferred_gid_index = i;  // First "::ffff:" GID index
                    logger->debug("-> Preferred GID [{}]: {}", i, gid_str);
                }

                // ignore an empty gid "fe80::"
                if (gid_str == "fe80::") {
                    logger->debug("-> Skip the empty gid [{}]: {}", i, gid_str);
                    continue;
                }

                // Last valid GID index
                if (gid_index == -1) {
                    gid_index = i;
                }
            }
        }
    }

    // Finally selected GID index
    if (preferred_gid_index != -1) {
        logger->info("Using preferred GID index: {}", preferred_gid_index);
        gid_index = preferred_gid_index;  // choose a high-priority GID index
    } else if (gid_index != -1) {
        logger->info("Using fallback GID index: {}", gid_index);
    } else {
        logger->error("No valid GID found!");
    }

    return gid_index;
}

static std::string make_destination_key(uint32_t lid,
                                        const union ibv_gid &gid) {
    // Example: convert lid to string + parse GID
    std::stringstream ss;
    ss << lid << "_" << parsed_gid((union ibv_gid *)&gid);
    return ss.str();
}

int get_traffic_class(struct rdma_context *ctx,
                      std::shared_ptr<spdlog::logger> logger) {
    // get traffic class value from pingweave.ini
    if (ctx->protocol == "ib") {
        if (IS_FAILURE(get_int_param_from_ini(ctx->traffic_class,
                                              "traffic_class_ib"))) {
            logger->error(
                "Failed to get a traffic class for IB from pingweave.ini. Use "
                "a default class: 0.");
            ctx->traffic_class = 0;
            return PINGWEAVE_FAILURE;
        }
    } else if (ctx->protocol == "roce") {
        if (IS_FAILURE(get_int_param_from_ini(ctx->traffic_class,
                                              "traffic_class_roce"))) {
            logger->error(
                "Failed to get a traffic class for RoCE from pingweave.ini. "
                "Use a default class: 0.");
            ctx->traffic_class = 0;
            return PINGWEAVE_FAILURE;
        }
    } else {
        logger->error("Invalid protocol type: {}", ctx->protocol);
        throw std::runtime_error(
            "Invalid protocol type - must be either ib or roce");
    }

    // success
    return PINGWEAVE_SUCCESS;
}

ibv_ah *get_or_create_ah(struct rdma_context *ctx, union rdma_addr rem_dest,
                         std::shared_ptr<spdlog::logger> logger) {
    // To avoid lookup slow-down, we clear the map if its size is too big
    if (ctx->ah_map.size() > MAX_NUM_HOSTS_IN_PINGLIST) {
        for (auto &pair : ctx->ah_map) {
            ibv_destroy_ah(pair.second);
        }
        ctx->ah_map.clear();
    }

    // Create a lookup key for AH
    auto key = make_destination_key(rem_dest.x.lid, rem_dest.x.gid);
    auto it = ctx->ah_map.find(key);
    if (it != ctx->ah_map.end()) {
        return it->second;  // reuse existing AH
    }

    // Create a new AH
    struct ibv_ah_attr ah_attr = {};
    ah_attr.dlid = rem_dest.x.lid;
    ah_attr.sl = SERVICE_LEVEL;
    ah_attr.port_num = ctx->active_port;
    ah_attr.is_global = 0;
    if (rem_dest.x.gid.global.interface_id) {  // For RoCEv2
        ah_attr.is_global = 1;
        ah_attr.grh.hop_limit = 255;
        ah_attr.grh.dgid = rem_dest.x.gid;
        ah_attr.grh.sgid_index = ctx->gid_index;
        ah_attr.grh.traffic_class = ctx->traffic_class;
    }

    ibv_ah *ah = ibv_create_ah(ctx->pd, &ah_attr);
    if (!ah) {
        logger->error("Failed to create AH for key {}", key);
        return nullptr;
    }

    ctx->ah_map[key] = ah;  // store to reuse
    return ah;
}

struct ibv_cq *pingweave_cq(struct rdma_context *ctx) {
    return ctx->rnic_hw_ts ? ibv_cq_ex_to_cq(ctx->cq_s.cq_ex) : ctx->cq_s.cq;
}

bool allocate_and_register_buffer(struct ibv_pd *pd, Buffer &buffer,
                                  size_t size,
                                  std::shared_ptr<spdlog::logger> logger) {
    // Allocate memory with proper alignment
    int page_size = sysconf(_SC_PAGESIZE);
    int ret = posix_memalign((void **)&buffer.addr, page_size, size);
    if (ret != 0 || !buffer.addr) {
        logger->error("Failed to allocate memory");
        return PINGWEAVE_FAILURE;
    }
    buffer.length = size;
    std::memset(buffer.addr, 0x0, size);  // initialize the buffer

    // Register the memory region with the RDMA device
    int access_flags = IBV_ACCESS_LOCAL_WRITE;  // For receive buffers
    buffer.mr = ibv_reg_mr(pd, buffer.addr, buffer.length, access_flags);
    if (!buffer.mr) {
        // Handle registration failure
        free(buffer.addr);
        logger->error("Failed to register memory region");
        return PINGWEAVE_FAILURE;
    }

    // success
    return PINGWEAVE_SUCCESS;
}

int init_ctx(struct rdma_context *ctx, const int &is_rx,
             std::shared_ptr<spdlog::logger> logger) {
    // initialize
    ctx->rnic_hw_ts = false;
    ctx->send_flags = IBV_SEND_SIGNALED;
    ctx->buf.resize(NUM_BUFFER);

    /** check RNIC timestamping support
        NOTE: This is a potential segmentation fault point.
     */
    struct ibv_device_attr_ex attrx;
    if (ibv_query_device_ex(ctx->context, NULL, &attrx)) {
        logger->info("Couldn't query device for extension features.");
    } else if (!attrx.completion_timestamp_mask) {
        logger->info("The device isn't completion timestamp capable.");
        ctx->completion_timestamp_mask = UINT64_MAX;  // for bit wrap-around
    } else {
        logger->info("RNIC HW timestamping is available.");
        ctx->rnic_hw_ts = true;
        ctx->completion_timestamp_mask = attrx.completion_timestamp_mask;
    }

    {  // find an active port
        int active_port = find_active_port(ctx, logger);
        if (active_port < 0) {
            logger->error("Unable to query port info for port: {}",
                          active_port);
            goto clean_device;
        }
        ctx->active_port = active_port;
    }

    {
        int gid_index = get_gid_table_index(ctx, logger);
        logger->debug("We use the last index {} among valid GID indices",
                      gid_index);
        if (gid_index == 0) {
            logger->info("-> probably, Infiniband device.");
        } else if (gid_index > 0) {
            logger->info("-> probably, RoCEv2 device.");
        } else {
            logger->info("GID {} is not available or something is wrong.",
                         gid_index);
            goto clean_device;
        }

        ctx->gid_index = gid_index;  // use last GID
    }

    {  // create a complete channel for event-driven polling
        ctx->channel = ibv_create_comp_channel(ctx->context);
        if (!ctx->channel) {
            logger->error("Couldn't create completion channel.");
            goto clean_device;
        }
    }

    {  // allocate a protection domain
        ctx->pd = ibv_alloc_pd(ctx->context);
        if (!ctx->pd) {
            logger->error("Couldn't allocate protection domain.");
            goto clean_comp_channel;
        }
    }

    {  // buffer allocation
        for (int i = 0; i < ctx->buf.size(); ++i) {
            if (IS_FAILURE(allocate_and_register_buffer(
                    ctx->pd, ctx->buf[i], MESSAGE_SIZE + GRH_SIZE, logger))) {
                logger->error("Failed to alloc/register memory");
                goto clean_pd;
            }
        }
    }

    {  // create CQ
        if (ctx->rnic_hw_ts) {
            struct ibv_cq_init_attr_ex attr_ex = {};
            attr_ex.cqe = NUM_BUFFER * (RX_DEPTH + TX_DEPTH);
            attr_ex.cq_context = NULL;
            attr_ex.channel = ctx->channel;
            attr_ex.comp_vector = 0;
            attr_ex.wc_flags =
                IBV_WC_EX_WITH_BYTE_LEN | IBV_WC_EX_WITH_COMPLETION_TIMESTAMP;
            ctx->cq_s.cq_ex = ibv_create_cq_ex(ctx->context, &attr_ex);
        } else {
            ctx->cq_s.cq =
                ibv_create_cq(ctx->context, NUM_BUFFER * (RX_DEPTH + TX_DEPTH),
                              NULL, ctx->channel, 0);
        }

        if (!pingweave_cq(ctx)) {
            logger->error("Couldn't create CQ");
            goto clean_mr;
        }
    }

    {  // create QP
        struct ibv_qp_attr attr = {};
        struct ibv_qp_init_attr init_attr = {};
        init_attr.send_cq = pingweave_cq(ctx);
        init_attr.recv_cq = pingweave_cq(ctx);
        init_attr.cap.max_send_wr = NUM_BUFFER * TX_DEPTH;
        init_attr.cap.max_recv_wr = NUM_BUFFER * RX_DEPTH;
        init_attr.cap.max_send_sge = 1;
        init_attr.cap.max_recv_sge = 1;
        init_attr.qp_type = IBV_QPT_UD;

        ctx->qp = ibv_create_qp(ctx->pd, &init_attr);
        if (!ctx->qp) {
            logger->error("Couldn't create QP");
            goto clean_cq;
        }

        ibv_query_qp(ctx->qp, &attr, IBV_QP_CAP, &init_attr);
        if (init_attr.cap.max_inline_data >= MESSAGE_SIZE + GRH_SIZE) {
            ctx->send_flags |= IBV_SEND_INLINE;
        }
    }

    {  // modify QP
        struct ibv_qp_attr attr = {};
        memset(&attr, 0, sizeof(attr));
        attr.qp_state = IBV_QPS_INIT;
        attr.pkey_index = 0;
        attr.port_num = ctx->active_port;
        attr.qkey = PINGWEAVE_REMOTE_QKEY;
        if (ibv_modify_qp(
                ctx->qp, &attr,
                IBV_QP_STATE | IBV_QP_PKEY_INDEX | IBV_QP_PORT | IBV_QP_QKEY)) {
            logger->error("Failed to modify QP to INIT");
            goto clean_qp;
        }
    }

    // success
    return PINGWEAVE_SUCCESS;

clean_qp:
    ibv_destroy_qp(ctx->qp);

clean_cq:
    ibv_destroy_cq(pingweave_cq(ctx));

clean_mr:
    for (int i = 0; i < ctx->buf.size(); ++i) {
        if (ctx->buf[i].mr) {
            ibv_dereg_mr(ctx->buf[i].mr);
        }
        if (ctx->buf[i].addr) {
            free(ctx->buf[i].addr);
        }
    }

clean_pd:
    ibv_dealloc_pd(ctx->pd);

clean_comp_channel:
    if (ctx->channel) {
        ibv_destroy_comp_channel(ctx->channel);
    }

clean_device:
    ibv_close_device(ctx->context);

    // failure
    return PINGWEAVE_FAILURE;
}

int prepare_ctx(struct rdma_context *ctx,
                std::shared_ptr<spdlog::logger> logger) {
    struct ibv_qp_attr attr = {};
    attr.qp_state = IBV_QPS_RTR;
    if (ibv_modify_qp(ctx->qp, &attr, IBV_QP_STATE)) {
        logger->error("Failed to modify QP to RTR");
        return PINGWEAVE_FAILURE;
    }

    attr.qp_state = IBV_QPS_RTS;
    attr.sq_psn = 0;

    if (ibv_modify_qp(ctx->qp, &attr, IBV_QP_STATE | IBV_QP_SQ_PSN)) {
        logger->error("Failed to modify QP to RTS");
        return PINGWEAVE_FAILURE;
    }

    // success
    return PINGWEAVE_SUCCESS;
}

int make_ctx(struct rdma_context *ctx, const std::string &ipv4,
             const std::string &protocol, const int &is_rx,
             std::shared_ptr<spdlog::logger> logger) {
    ctx->ipv4 = ipv4;          // ipv4 address
    ctx->is_rx = is_rx;        // rx or tx
    ctx->protocol = protocol;  // ib or roce

    if (IS_FAILURE(get_context_by_ip(ctx, logger))) {
        return PINGWEAVE_FAILURE;  // propagate error
    }

    if (IS_FAILURE(init_ctx(ctx, is_rx, logger))) {
        return PINGWEAVE_FAILURE;  // propagate error
    }

    if (IS_FAILURE(prepare_ctx(ctx, logger))) {
        return PINGWEAVE_FAILURE;  // propagate error
    }

    if (ibv_query_gid(ctx->context, ctx->active_port, ctx->gid_index,
                      &ctx->gid)) {
        logger->error("Could not get my gid for gid index {}", ctx->gid_index);
        return PINGWEAVE_FAILURE;  // propagate error
    }

    // gid to wired and parsed gid
    gid_to_wire_gid(&ctx->gid, ctx->wired_gid);
    inet_ntop(AF_INET6, &ctx->gid, ctx->parsed_gid, sizeof(ctx->parsed_gid));

    // get a traffic class
    get_traffic_class(ctx, logger);

    std::string ctx_send_type = is_rx ? "RX" : "TX";
    logger->info("[{}] IP: {} has Queue pair with GID: {}, QPN: {}",
                 ctx_send_type, ipv4, ctx->parsed_gid, ctx->qp->qp_num);

    // success
    return PINGWEAVE_SUCCESS;
}

// Function to initialize RDMA contexts
int initialize_contexts(struct rdma_context &ctx_tx,
                        struct rdma_context &ctx_rx, const std::string &ipv4,
                        const std::string &protocol,
                        std::shared_ptr<spdlog::logger> logger) {
    if (IS_FAILURE(make_ctx(&ctx_tx, ipv4, protocol, false, logger))) {
        logger->error("Failed to create TX context for IP: {}", ipv4);
        return PINGWEAVE_FAILURE;
    }
    if (IS_FAILURE(make_ctx(&ctx_rx, ipv4, protocol, true, logger))) {
        logger->error("Failed to create RX context for IP: {}", ipv4);
        return true;
    }
    return PINGWEAVE_SUCCESS;
}

int post_recv(struct rdma_context *ctx, const uint64_t &wr_id, const int &n) {
    // buffer index from wr_id
    auto buf_idx = wr_id % ctx->buf.size();

    /* generate a unique wr_id */
    struct ibv_sge list = {};
    auto buf = ctx->buf[buf_idx];
    list.addr = (uintptr_t)buf.addr;
    list.length = buf.length;
    list.lkey = buf.mr->lkey;

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

int post_send(struct rdma_context *ctx, union rdma_addr rem_dest,
              const char *msg, const size_t &msg_len, const int &buf_idx,
              const uint64_t &wr_id, std::shared_ptr<spdlog::logger> logger) {
    ibv_ah *ah = get_or_create_ah(ctx, rem_dest, logger);
    if (!ah) {
        logger->error("Failed to create or reuse an AH");
        return PINGWEAVE_FAILURE;
    }

    // Prepare ibv_send_wr
    struct ibv_sge list = {};
    auto &buf = ctx->buf[buf_idx];
    memcpy(buf.addr + GRH_SIZE, msg, msg_len);
    list.addr = (uintptr_t)(buf.addr + GRH_SIZE);
    list.length = msg_len;
    list.lkey = buf.mr->lkey;

    struct ibv_send_wr wr = {};
    wr.wr_id = wr_id;
    wr.sg_list = &list;
    wr.num_sge = 1;
    wr.opcode = IBV_WR_SEND;
    wr.send_flags = ctx->send_flags;
    wr.wr.ud.ah = ah;  // REUSED handle
    wr.wr.ud.remote_qpn = rem_dest.x.qpn;
    wr.wr.ud.remote_qkey = PINGWEAVE_REMOTE_QKEY;

    struct ibv_send_wr *bad_wr = nullptr;
    if (ibv_post_send(ctx->qp, &wr, &bad_wr)) {
        logger->error("ibv_post_send(...) failed");
        return PINGWEAVE_FAILURE;
    }
    return PINGWEAVE_SUCCESS;
}

// Utility function: Wait for CQ event and handle it
int wait_for_cq_event(struct rdma_context *ctx,
                      std::shared_ptr<spdlog::logger> logger) {
    struct ibv_cq *ev_cq;
    void *ev_ctx;

    if (ibv_get_cq_event(ctx->channel, &ev_cq, &ev_ctx)) {
        logger->error("Failed to get cq_event");
        return PINGWEAVE_FAILURE;
    }

    // Verify that the event is from the correct CQ
    if (ev_cq != pingweave_cq(ctx)) {
        logger->error("CQ event for unknown CQ");
        return PINGWEAVE_FAILURE;
    }

    return PINGWEAVE_SUCCESS;
}

// save RDMA device's Server RX QP information
int save_device_info(struct rdma_context *ctx,
                     std::shared_ptr<spdlog::logger> logger) {
    const std::string directory = get_src_dir() + DIR_UPLOAD_PATH;
    struct stat st = {0};

    if (stat(directory.c_str(), &st) == -1) {
        // create a directory if not exists
        if (mkdir(directory.c_str(), 0744) != 0) {
            logger->error("Cannot create a directory {}", directory);
            return PINGWEAVE_FAILURE;
        }
    }

    // 2. compose a file name
    std::string filename = directory + "/" + ctx->ipv4;

    // 3. save (overwrite)
    std::ofstream outfile(filename);
    if (!outfile.is_open()) {
        logger->error("Cannot open a file {} ({})", filename, strerror(errno));
        return PINGWEAVE_FAILURE;
    }

    // 4. get a current time
    std::string now = get_current_timestamp_system_str();

    // 5. save as lines (GID, LID, QPN, TIME)
    outfile << ctx->wired_gid << "\n"
            << ctx->portinfo.lid << "\n"
            << ctx->qp->qp_num << "\n"
            << now;  // GID, LID, QPN, TIME

    // check error
    if (!outfile) {
        logger->error("Error occued when writing a file {} ({})", filename,
                      strerror(errno));
        return PINGWEAVE_FAILURE;
    }

    outfile.close();
    return PINGWEAVE_SUCCESS;
}


std::string convert_rdma_result_to_str(const std::string &srcip,
                                       const std::string &dstip,
                                       const rdma_result_info_t &result_info,
                                       const result_stat_t &client_stat,
                                       const result_stat_t &network_stat,
                                       const result_stat_t &server_stat) {
    std::stringstream ss;
    ss << srcip << "," << dstip << ","
       << timestamp_ns_to_string(result_info.ts_start) << ","
       << timestamp_ns_to_string(result_info.ts_end) << ","
       << result_info.n_success << "," << result_info.n_failure << ","
       << result_info.n_weird << ","
       << "client," << client_stat.mean << "," << client_stat.max << ","
       << client_stat.percentile_50 << "," << client_stat.percentile_95 << ","
       << client_stat.percentile_99 << ","
       << "network," << network_stat.mean << "," << network_stat.max << ","
       << network_stat.percentile_50 << "," << network_stat.percentile_95 << ","
       << network_stat.percentile_99 << ","
       << "server," << server_stat.mean << "," << server_stat.max << ","
       << server_stat.percentile_50 << "," << server_stat.percentile_95 << ","
       << server_stat.percentile_99;

    // string 저장
    return ss.str();
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

// use when reading from file
void gid_to_wire_gid(const union ibv_gid *gid, char wgid[]) {
    uint32_t tmp_gid[4];
    int i;

    memcpy(tmp_gid, gid, sizeof(tmp_gid));
    for (i = 0; i < 4; ++i) {
        sprintf(&wgid[i * 8], "%08x", htobe32(tmp_gid[i]));
    }
}

// GID parser
std::string parsed_gid(union ibv_gid *gid) {
    char parsed_gid[33];
    inet_ntop(AF_INET6, gid, parsed_gid, sizeof(parsed_gid));
    return std::string(parsed_gid);
}