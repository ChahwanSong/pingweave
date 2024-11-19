#include "rdma_common.hpp"

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

int get_context_by_ip(struct pingweave_context *ctx,
                      std::shared_ptr<spdlog::logger> logger) {
    struct ifaddrs *ifaddr, *ifa;
    int family;
    if (getifaddrs(&ifaddr) == -1) {
        logger->error("Failed to getifaddrs");
        return true;
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
                return true;
            }

            ctx->iface = std::string(ifa->ifa_name);
            if (strcmp(host, ctx->ipv4.c_str()) == 0) {
                if (get_context_by_ifname(ifa->ifa_name, ctx)) {
                    logger->error(
                        "No matching RDMA device found for interface: {}",
                        ifa->ifa_name);
                    return true;
                } else {
                    break;
                }
            }
        }
    }

    freeifaddrs(ifaddr);
    if (!ctx->context) {
        logger->error("No matching RDMA device found for IP {}", ctx->ipv4);
        return true;
    }

    // success
    return false;
}

std::set<std::string> get_all_local_ips() {
    std::set<std::string> local_ips;
    struct ifaddrs *interfaces, *ifa;
    char ip[INET_ADDRSTRLEN];

    if (getifaddrs(&interfaces) == -1) {
        std::cerr << "Error getting interfaces." << std::endl;
        return local_ips;
    }

    for (ifa = interfaces; ifa != nullptr; ifa = ifa->ifa_next) {
        if (ifa->ifa_addr == nullptr) continue;

        std::string ifa_name = ifa->ifa_name;

        // Ignore interfaces starting with "virbr", "docker" or same as "lo"
        if (ifa_name == "lo" || ifa_name.rfind("virbr", 0) == 0 ||
            ifa_name.rfind("docker", 0) == 0) {
            continue;
        }

        if (ifa->ifa_addr->sa_family == AF_INET) {
            struct sockaddr_in *addr =
                reinterpret_cast<struct sockaddr_in *>(ifa->ifa_addr);
            inet_ntop(AF_INET, &(addr->sin_addr), ip, INET_ADDRSTRLEN);
            local_ips.insert(ip);
        }
    }

    freeifaddrs(interfaces);
    return local_ips;
}

// If error occurs, myaddr returned is empty set.
void get_my_addr(const std::string &filename, std::set<std::string> &myaddr) {
    fkyaml::node config;
    myaddr.clear();  // clean-slate start

    try {
        std::ifstream ifs(filename);
        config = fkyaml::node::deserialize(ifs);
        spdlog::debug("Pinglist.yaml loaded successfully.");
    } catch (const std::exception &e) {
        spdlog::error(
            "An unexpected error occurred while loading the file {}: {}",
            filename, e.what());
        return;
    }

    try {
        // Get the RDMA category groups
        if (!config.contains("rdma")) {
            spdlog::error("No 'rdma' category found in the YAML file.");
            return;
        }

        // Retrieve the node's IP addr
        std::set<std::string> local_ips = get_all_local_ips();

        // find all my ip addrs is in pinglist ip addrs
        for (auto &group : config["rdma"]) {
            for (auto &ip : group) {
                // If IP is on the current node, add it
                std::string ip_addr = ip.get_value_ref<std::string &>();
                if (local_ips.find(ip_addr) != local_ips.end()) {
                    myaddr.insert(ip_addr);
                }
            }
        }
    } catch (const std::exception &e) {
        spdlog::error("Failed to get my IP addresses from pinglist.yaml");
    }
}

// Find a valid active port on RDMA devices
int find_active_port(struct pingweave_context *ctx) {
    ibv_device_attr device_attr;
    if (ibv_query_device(ctx->context, &device_attr)) {
        fprintf(stderr, "Failed to query device");
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

// thread ID and cast to string
std::string get_thread_id() {
    std::stringstream ss;
    ss << std::this_thread::get_id();
    return ss.str();
}

struct ibv_cq *pingweave_cq(struct pingweave_context *ctx) {
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
        return false;
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
        return false;
    }

    // success
    return true;
}

int init_ctx(struct pingweave_context *ctx, const int &is_rx,
             std::shared_ptr<spdlog::logger> logger) {
    // initialize
    ctx->rnic_hw_ts = false;
    ctx->send_flags = IBV_SEND_SIGNALED;
    ctx->buf.resize(NUM_BUFFER);

    /* check RNIC timestamping support */
    struct ibv_device_attr_ex attrx;
    if (ibv_query_device_ex(ctx->context, NULL, &attrx)) {
        logger->info("Couldn't query device for extension features.");
    } else if (!attrx.completion_timestamp_mask) {
        logger->info("The device isn't completion timestamp capable.");
    } else {
        logger->info("RNIC HW timestamping is available.");
        ctx->rnic_hw_ts = true;
        ctx->completion_timestamp_mask = attrx.completion_timestamp_mask;
    }

    {  // find an active port
        int active_port = find_active_port(ctx);
        if (active_port < 0) {
            logger->error("Unable to query port info for port: {}",
                          active_port);
            goto clean_device;
        }
        ctx->active_port = active_port;
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
            if (!allocate_and_register_buffer(
                    ctx->pd, ctx->buf[i], MESSAGE_SIZE + GRH_SIZE, logger)) {
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
    return false;

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
    return true;
}

int prepare_ctx(struct pingweave_context *ctx,
                std::shared_ptr<spdlog::logger> logger) {
    struct ibv_qp_attr attr = {};
    attr.qp_state = IBV_QPS_RTR;
    if (ibv_modify_qp(ctx->qp, &attr, IBV_QP_STATE)) {
        logger->error("Failed to modify QP to RTR");
        return true;
    }

    attr.qp_state = IBV_QPS_RTS;
    attr.sq_psn = 0;

    if (ibv_modify_qp(ctx->qp, &attr, IBV_QP_STATE | IBV_QP_SQ_PSN)) {
        logger->error("Failed to modify QP to RTS");
        return true;
    }

    // success
    return false;
}

int make_ctx(struct pingweave_context *ctx, const std::string &ipv4,
             const int &is_rx, std::shared_ptr<spdlog::logger> logger) {
    ctx->ipv4 = ipv4;
    ctx->is_rx = is_rx;
    if (get_context_by_ip(ctx, logger)) {
        return true;  // propagate error
    }

    if (init_ctx(ctx, is_rx, logger)) {
        return true;  // propagate error
    }

    if (prepare_ctx(ctx, logger)) {
        return true;  // propagate error
    }

    if (ibv_query_gid(ctx->context, ctx->active_port, GID_INDEX, &ctx->gid)) {
        logger->error("Could not get my gid for gid index {}", GID_INDEX);
        return true;  // propagate error
    }

    // sanity check - always use GID
    logger->debug("ctx->gid.global.subnet_prefix: {}",
              ctx->gid.global.subnet_prefix);
    // if (ctx->gid.global.subnet_prefix == 0) {
    //     logger->error("GID subnet prefix must be non-zero.");
    //     return true;  // propagate error
    // }

    // gid to wired and parsed gid
    gid_to_wire_gid(&ctx->gid, ctx->wired_gid);
    inet_ntop(AF_INET6, &ctx->gid, ctx->parsed_gid, sizeof(ctx->parsed_gid));

    std::string ctx_send_type = is_rx ? "RX" : "TX";
    logger->info("[{}] IP: {} has Queue pair with GID: {}, QPN: {}",
                 ctx_send_type, ipv4, ctx->parsed_gid, ctx->qp->qp_num);

    // success
    return false;
}

// Function to initialize RDMA contexts
int initialize_contexts(pingweave_context &ctx_tx, pingweave_context &ctx_rx,
                        const std::string &ipv4,
                        std::shared_ptr<spdlog::logger> logger) {
    if (make_ctx(&ctx_tx, ipv4, false, logger)) {
        logger->error("Failed to create TX context for IP: {}", ipv4);
        return true;
    }
    if (make_ctx(&ctx_rx, ipv4, true, logger)) {
        logger->error("Failed to create RX context for IP: {}", ipv4);
        return true;
    }
    return false;
}

int post_recv(struct pingweave_context *ctx, const uint64_t &wr_id,
              const int &n) {
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

int post_send(struct pingweave_context *ctx, union rdma_addr rem_dest,
              const char *msg, const size_t &msg_len, const int &buf_idx,
              const uint64_t &wr_id, std::shared_ptr<spdlog::logger> logger) {
    struct ibv_ah_attr ah_attr = {};
    ah_attr.is_global = 0;
    ah_attr.dlid = rem_dest.x.lid;
    ah_attr.sl = SERVICE_LEVEL;
    ah_attr.src_path_bits = 0;
    ah_attr.port_num = ctx->active_port;
    logger->debug("post_send: port_num is {}", ctx->active_port);
    logger->debug("post_send: gid.global.interface_id is {}",
                    rem_dest.x.gid.global.interface_id);

    if (rem_dest.x.gid.global.interface_id) { // IP addr for RoCEv2
        ah_attr.is_global = 1;
        ah_attr.grh.hop_limit = 1;
        ah_attr.grh.dgid = rem_dest.x.gid;
        ah_attr.grh.sgid_index = GID_INDEX;
        ah_attr.grh.traffic_class = RDMA_TRAFFIC_CLASS;
    } else {
        logger->error("PingWeave must use GID interface");
        return true;
    }

    // address handle
    struct ibv_ah *ah = ibv_create_ah(ctx->pd, &ah_attr);

    // sanity check
    if (!ah) {
        logger->error("Failed to create AH");
        return true;
    }
    if (!msg) {
        logger->error("Empty message");
        return true;
    }

    /* save a message to buffer */
    assert(buf_idx < ctx->buf.size());  // sanity check
    auto buf = ctx->buf[buf_idx];
    std::memcpy(buf.addr + GRH_SIZE, msg, msg_len);

    /* generate a unique wr_id */
    struct ibv_sge list = {};
    list.addr = (uintptr_t)buf.addr + GRH_SIZE;
    list.length = buf.length - GRH_SIZE;
    list.lkey = buf.mr->lkey;

    struct ibv_send_wr wr = {};
    wr.wr_id = wr_id;
    wr.sg_list = &list;
    wr.num_sge = 1;
    wr.opcode = IBV_WR_SEND;
    wr.send_flags = ctx->send_flags;
    wr.wr.ud.ah = ah;
    wr.wr.ud.remote_qpn = rem_dest.x.qpn;
    wr.wr.ud.remote_qkey = PINGWEAVE_REMOTE_QKEY;
    struct ibv_send_wr *bad_wr;

    if (ibv_post_send(ctx->qp, &wr, &bad_wr)) {
        logger->error("SEND post is failed");
        return true;
    }

    // success
    return false;
}

// Utility function: Wait for CQ event and handle it
int wait_for_cq_event(struct pingweave_context *ctx,
                      std::shared_ptr<spdlog::logger> logger) {
    struct ibv_cq *ev_cq;
    void *ev_ctx;

    if (ibv_get_cq_event(ctx->channel, &ev_cq, &ev_ctx)) {
        logger->error("Failed to get cq_event");
        return false;
    }

    // Verify that the event is from the correct CQ
    if (ev_cq != pingweave_cq(ctx)) {
        logger->error("CQ event for unknown CQ");
        return false;
    }

    // // Acknowledge the CQ event
    // ibv_ack_cq_events(pingweave_cq(ctx), 1);

    // // Re-register for CQ event notifications
    // if (ibv_req_notify_cq(pingweave_cq(ctx), 0)) {
    //     logger->error("Couldn't register CQE notification");
    //     return false;
    // }

    return true;
}

// calculate stats from delay histroy
result_stat_t calc_stats(const std::vector<uint64_t> &delays) {
    if (delays.empty()) {
        // 벡터가 비어 있을 때 -1 반환
        return {static_cast<uint64_t>(0), static_cast<uint64_t>(0),
                static_cast<uint64_t>(0), static_cast<uint64_t>(0),
                static_cast<uint64_t>(0)};
    }

    // 평균(mean)
    uint64_t sum = std::accumulate(delays.begin(), delays.end(), uint64_t(0));
    uint64_t mean = sum / delays.size();

    // 최대값(max)
    uint64_t max = *std::max_element(delays.begin(), delays.end());

    // 백분위수를 계산하기 위해 벡터를 정렬
    std::vector<uint64_t> sorted_delays = delays;
    std::sort(sorted_delays.begin(), sorted_delays.end());

    // 중간값(50-percentile), 95-percentile, 99-percentile 인덱스 계산
    uint64_t percentile_50 = sorted_delays[sorted_delays.size() * 50 / 100];
    uint64_t percentile_95 = sorted_delays[sorted_delays.size() * 95 / 100];
    uint64_t percentile_99 = sorted_delays[sorted_delays.size() * 99 / 100];

    return {mean, max, percentile_50, percentile_95, percentile_99};
}

int save_device_info(struct pingweave_context *ctx,
                     std::shared_ptr<spdlog::logger> logger) {
    const std::string directory = get_source_directory() + DIR_UPLOAD_PATH;
    struct stat st = {0};

    if (stat(directory.c_str(), &st) == -1) {
        // create a directory if not exists
        if (mkdir(directory.c_str(), 0744) != 0) {
            logger->error("Cannot create a directory {}", directory);
            return 1;
        }
    }

    // 2. compose a file name
    std::string filename = directory + "/" + ctx->ipv4;

    // 3. save (overwrite)
    std::ofstream outfile(filename);
    if (!outfile.is_open()) {
        logger->error("Cannot open a file {} ({})", filename, strerror(errno));
        return 1;
    }

    // 4. get a current time
    std::string now = get_current_timestamp_string();

    // 5. save as lines (GID, LID, QPN, TIME)
    outfile << ctx->wired_gid << "\n"
            << ctx->portinfo.lid << "\n"
            << ctx->qp->qp_num << "\n"
            << now;  // GID, LID, QPN, TIME

    // check error
    if (!outfile) {
        logger->error("Error occued when writing a file {} ({})", filename,
                      strerror(errno));
        return 1;
    }

    outfile.close();
    return 0;
}

int load_device_info(union rdma_addr *dst_addr, const std::string &filepath,
                     std::shared_ptr<spdlog::logger> logger) {
    std::string line, gid, lid, qpn;

    std::ifstream file(filepath);
    if (!file.is_open()) {
        logger->error("Error opening file.");
        return 1;
    }

    // read gid
    if (std::getline(file, line)) {
        gid = line;
    } else {
        logger->error("Error reading first line.");
        return 1;
    }

    // read lid
    if (std::getline(file, line)) {
        lid = line;
    } else {
        logger->error("Error reading second line.");
        return 1;
    }

    // read qpn
    if (std::getline(file, line)) {
        qpn = line;
    } else {
        logger->error("Error reading second line.");
        return 1;
    }

    file.close();

    try {
        wire_gid_to_gid(gid.c_str(), &dst_addr->x.gid);
        dst_addr->x.lid = std::stoi(lid);
        dst_addr->x.qpn = std::stoi(qpn);
    } catch (const std::invalid_argument &e) {
        logger->error("Invalid argument: {}", e.what());
        return 1;
    } catch (const std::out_of_range &e) {
        logger->error("Out of range: {}", e.what());
        return 1;
    }
    return 0;
}
