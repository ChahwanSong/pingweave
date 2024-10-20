#include "common.hpp"

static struct ibv_cq *pp_cq(struct pingpong_context *ctx) {
    return rnic_hw_ts ? ibv_cq_ex_to_cq(ctx->cq_s.cq_ex) : ctx->cq_s.cq;
}

int init_ctx(struct pingpong_context *ctx) {
    /* check RNIC timestamping support */
    struct ibv_device_attr_ex attrx;
    if (ibv_query_device_ex(ctx->context, NULL, &attrx)) {
        printf("[WARNING] Couldn't query device for extension features\n");
    } else if (!attrx.completion_timestamp_mask) {
        printf("[WARNING] -> The device isn't completion timestamp capable\n");
    } else {
        printf("Use RNIC Timestamping...\n");
        rnic_hw_ts = 1;
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
    ctx->rx_depth = rx_depth;

    if (posix_memalign((void **)&ctx->buf, page_size, msg_size + grh_size)) {
        std::cerr << "ctx->buf memalign failed\n" << std::endl;
        exit(1);
    }
    memset(ctx->buf, 0x7b, msg_size + grh_size);

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
        if (use_event) {
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
        ctx->mr = ibv_reg_mr(ctx->pd, ctx->buf, msg_size + grh_size,
                             IBV_ACCESS_LOCAL_WRITE);
        if (!ctx->mr) {
            fprintf(stderr, "Couldn't register MR\n");
            goto clean_pd;
        }
    }

    {
        if (rnic_hw_ts) {
            struct ibv_cq_init_attr_ex attr_ex = {};
            attr_ex.cqe = rx_depth + 1;
            attr_ex.cq_context = NULL;
            attr_ex.channel = ctx->channel;
            attr_ex.comp_vector = 0;
            attr_ex.wc_flags =
                IBV_WC_EX_WITH_BYTE_LEN | IBV_WC_EX_WITH_COMPLETION_TIMESTAMP;
            ctx->cq_s.cq_ex = ibv_create_cq_ex(ctx->context, &attr_ex);
        } else {
            ctx->cq_s.cq = ibv_create_cq(ctx->context, rx_depth + 1, NULL,
                                         ctx->channel, 0);
        }

        if (!pp_cq(ctx)) {
            fprintf(stderr, "Couldn't create CQ\n");
            goto clean_mr;
        }
    }

    {
        struct ibv_qp_attr attr;
        struct ibv_qp_init_attr init_attr = {};
        init_attr.send_cq = pp_cq(ctx);
        init_attr.recv_cq = pp_cq(ctx);
        init_attr.cap.max_send_wr = 3;
        init_attr.cap.max_recv_wr = rx_depth;
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
    ibv_destroy_cq(pp_cq(ctx));

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

static int connect_ctx(struct pingpong_context *ctx) {
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

static int post_recv(struct pingpong_context *ctx, int n) {
    /* generate a unique wr_id */
    uint64_t randval = static_cast<uint64_t>(lrand48() % UINT8_MAX) * 1000;
    printf("  [POST] RECV with wr_id: %lu\n", randval);
    struct ibv_sge list = {};
    list.addr = (uintptr_t)ctx->buf;
    list.length = msg_size + grh_size;
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

static int post_send(struct pingpong_context *ctx,
                     struct pingpong_dest rem_dest, std::string msg) {
    struct ibv_ah_attr ah_attr = {};
    ah_attr.is_global = 0;
    ah_attr.dlid = rem_dest.lid;
    ah_attr.sl = service_level;
    ah_attr.src_path_bits = 0;
    ah_attr.port_num = ctx->active_port;

    if (rem_dest.gid.global.interface_id) {
        ah_attr.is_global = 1;
        ah_attr.grh.hop_limit = 1;
        ah_attr.grh.dgid = rem_dest.gid;
        ah_attr.grh.sgid_index = gid_index;
    }

    ctx->ah = ibv_create_ah(ctx->pd, &ah_attr);
    if (!ctx->ah) {
        fprintf(stderr, "Failed to create AH\n");
        return 1;
    }

    /* save a message to buffer */
    strcpy(ctx->buf + grh_size, msg.c_str());

    /* generate a unique wr_id */
    uint64_t randval = static_cast<uint64_t>(lrand48() % UINT8_MAX);
    printf("  [POST] SEND with wr_id: %lu\n", randval);
    struct ibv_sge list = {};
    list.addr = (uintptr_t)ctx->buf + grh_size;
    list.length = msg_size;
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

int main(int argc, char *argv[]) {
    struct timeval start, end;
    struct pingpong_context *ctx = new pingpong_context();

    if (argc < 2 || argc > 3) {
        std::cerr << "Usage: " << argv[0] << " [-s] <Server IP>" << std::endl;
        return 1;
    }

    ctx->is_server = (argc == 3 && strcmp(argv[1], "-s") == 0) ? 1 : 0;

    if (argc == 3 && ctx->is_server == 0) {
        std::cerr << "Error: Unknown flag '" << argv[1] << "'" << std::endl;
        return 1;
    }

    const char *target_ip = (ctx->is_server == 1) ? argv[2] : argv[1];

    printf("------ Launch RDMA UD Pingpong ------\n");
    srand48(getpid() * time(NULL));

    struct pingpong_dest my_dest;
    struct pingpong_dest rem_dest;
    int ret;
    char parsed_gid[33];
    char wired_gid[33];

    // RDMA 장치 찾기
    ctx->context = get_context_by_ip(target_ip);
    printf("Device: %s\n", ctx->context->device->name);

    // context 초기화
    if (init_ctx(ctx)) { /* failed */
        exit(1);
    }

    if (connect_ctx(ctx)) {
        fprintf(stderr, "Couldn't modify QP state\n");
        exit(1);
    }

    // Request CQ Notification
    if (use_event) {
        if (ibv_req_notify_cq(pp_cq(ctx), 0)) {
            fprintf(stderr, "Couldn't request CQ notification\n");
            exit(1);
        }
    }

    my_dest.lid = ctx->portinfo.lid;
    my_dest.qpn = ctx->qp->qp_num;
    if (ibv_query_gid(ctx->context, ctx->active_port, gid_index,
                      &my_dest.gid)) {
        fprintf(stderr, "Could not get local gid for gid index %d\n",
                gid_index);
        exit(1);
    }

    inet_ntop(AF_INET6, &my_dest.gid, parsed_gid, sizeof parsed_gid);
    printf("  local info:  IP %s, LID %d, QPN %d, GID %s\n", target_ip,
           my_dest.lid, my_dest.qpn, parsed_gid);

    // deparse for csv file
    gid_to_wire_gid(&my_dest.gid, wired_gid);

    // 내 정보 저장하기
    put_local_info(&my_dest, ctx->is_server, target_ip);

    // wait until fill up the server/client information
    printf(
        "\n  >> Press Enter after putting server info to "
        "local_server_info.csv\n");
    getchar();

    // 상대편 정보 읽어오기
    get_local_info(&rem_dest, ctx->is_server);

    struct ibv_poll_cq_attr attr = {};
    if (rnic_hw_ts) {
        ret = ibv_start_poll(ctx->cq_s.cq_ex, &attr);
        assert(ret == ENOENT);
    }

    // RECV WR 포스트
    int num_init_post = 1;
    ret = post_recv(ctx, num_init_post);
    if (ret < num_init_post) {
        fprintf(stderr, "Failed to post all RECV (%d/%d)\n", ret,
                num_init_post);
    }

    if (ctx->is_server) {  // 서버

        int cnt_send = 0;  // expect total 2 SENDs
        int num_cqe = 0;   // number of polled CQE
        uint64_t ts_cqe = 0, ts_server_recv = 0, ts_server_send = 0;
        struct ibv_wc wc = {};

        while (true) {
            if (cnt_send == 1) {
                /** The second SEND (a.k.a. ACK) must be sent after first
                 * post_send's CQE. */
                /** TODO: Write the elapsed time to ACK's buffer
                 * and send the information */
                // usleep(1);
                std::string ack_msg = msg_from_server + std::string("_2");
                if (rnic_hw_ts) {
                    ack_msg = std::to_string(ts_server_send - ts_server_recv);
                    printf("Server process time: %s\n", ack_msg.c_str());
                }

                if (post_send(ctx, rem_dest, ack_msg)) {
                    fprintf(stderr, "Couldn't post send\n");
                    exit(1);
                }
            }

            /* if using complete channel */
            if (use_event) {
                struct ibv_cq *ev_cq;
                void *ev_ctx;
                if (ibv_get_cq_event(ctx->channel, &ev_cq, &ev_ctx)) {
                    fprintf(stderr, "Failed to get cq_event\n");
                    return 1;
                }

                ibv_ack_cq_events(pp_cq(ctx), 1);

                if (ev_cq != pp_cq(ctx)) {
                    fprintf(stderr, "CQ event for unknown CQ %p\n", ev_cq);
                    return 1;
                }
                if (ibv_req_notify_cq(pp_cq(ctx), 0)) {
                    fprintf(stderr, "Couldn't request CQ notification\n");
                    return 1;
                }
            }

            /* polling CQE */
            if (rnic_hw_ts) {  // extension
                do {
                    ret = ibv_next_poll(ctx->cq_s.cq_ex);
                } while (!use_event && ret == ENOENT);  // until empty
                num_cqe = 1;
                ts_cqe = ibv_wc_read_completion_ts(ctx->cq_s.cq_ex);
            } else {  // original
                do {
                    num_cqe = ibv_poll_cq(pp_cq(ctx), 1, &wc);
                } while (!use_event && num_cqe == 0);  // until empty

                if (num_cqe < 0) {
                    std::cerr << "Poll CQ failed" << std::endl;
                    break;
                } else if (num_cqe > 1) {
                    std::cerr << "CQE must be 1" << std::endl;
                    break;
                }
            }

            if (num_cqe > 0) {
                printf("* CQE Event happend! (%d)\n", num_cqe);

                if (rnic_hw_ts) {
                    /** TODO: overrided for simplicity. Later, optimize it */
                    wc = {0};
                    wc.status = ctx->cq_s.cq_ex->status;
                    wc.wr_id = ctx->cq_s.cq_ex->wr_id;
                    wc.byte_len = ibv_wc_read_byte_len(ctx->cq_s.cq_ex);
                    wc.opcode = ctx->cq_s.cq_ex->read_opcode(ctx->cq_s.cq_ex);
                }

                if (wc.status == IBV_WC_SUCCESS) {
                    printf("  [CQE] wc.status: %s(%d)\n",
                           ibv_wc_status_str(wc.status), wc.status);

                    if (wc.opcode == IBV_WC_RECV) {
                        if (rnic_hw_ts) {
                            ts_server_recv = ts_cqe; /* timestamp */
                        }

                        ret = post_recv(ctx, 1);  // 다시 수신 대기
                        printf("  [CQE] RECV (wr_id: %lu)\n", wc.wr_id);
                        printf("Server received %d bytes. Buffer: %s\n",
                               wc.byte_len, ctx->buf + grh_size);

                        /* Post a send with a message */
                        if (post_send(ctx, rem_dest,
                                      msg_from_server + std::string("_1"))) {
                            fprintf(stderr, "Couldn't post send\n");
                            exit(1);
                        }
                    }

                    if (wc.opcode == IBV_WC_SEND) {
                        printf("  [CQE] SEND (wr_id: %lu)\n", wc.wr_id);
                        ++cnt_send;
                        if (cnt_send == 1) {
                            if (rnic_hw_ts) {
                                ts_server_send = ts_cqe; /* timestamp */
                            }
                        } else if (cnt_send >= 2) {
                            printf("Server completed.\n");
                            exit(1);
                        }
                    }
                } else {
                    perror("Failed to get IBV_WC_SUCCESS\n");
                    exit(1);
                }
            }
        }

        return 0;

    } else {  // Client

        int cnt_recv = 0;  // expect total 2 RECVs
        int num_cqe = 0;   // number of polled CQE
        uint64_t ts_cqe = 0, ts_client_send = 0, ts_client_recv = 0;
        struct ibv_wc wc = {};

        if (gettimeofday(&start, NULL)) {
            perror("gettimeofday");
            return 1;
        }

        // Client first send a message to server
        if (post_send(ctx, rem_dest, msg_from_client)) {
            fprintf(stderr, "Couldn't post send\n");
            exit(1);
        }

        while (true) {
            if (use_event) {
                struct ibv_cq *ev_cq;
                void *ev_ctx;
                if (ibv_get_cq_event(ctx->channel, &ev_cq, &ev_ctx)) {
                    fprintf(stderr, "Failed to get cq_event\n");
                    return 1;
                }

                ibv_ack_cq_events(pp_cq(ctx), 1);

                if (ev_cq != pp_cq(ctx)) {
                    fprintf(stderr, "CQ event for unknown CQ %p\n", ev_cq);
                    return 1;
                }
                if (ibv_req_notify_cq(pp_cq(ctx), 0)) {
                    fprintf(stderr, "Couldn't request CQ notification\n");
                    return 1;
                }
            }

            /* polling CQE */
            if (rnic_hw_ts) {  // extension
                do {
                    ret = ibv_next_poll(ctx->cq_s.cq_ex);
                } while (!use_event && ret == ENOENT);  // until empty
                num_cqe = 1;
                ts_cqe = ibv_wc_read_completion_ts(ctx->cq_s.cq_ex);
            } else {  // original
                do {
                    num_cqe = ibv_poll_cq(pp_cq(ctx), 1, &wc);
                } while (!use_event && num_cqe == 0);  // until empty

                if (num_cqe < 0) {
                    std::cerr << "Poll CQ failed" << std::endl;
                    break;
                } else if (num_cqe > 1) {
                    std::cerr << "CQE must be 1" << std::endl;
                    break;
                }
            }

            if (num_cqe > 0) {
                printf("* CQE Event happend! (%d)\n", num_cqe);

                if (rnic_hw_ts) {
                    /** TODO: overrided for simplicity. Later, optimize it */
                    // wc = {0};
                    wc.status = ctx->cq_s.cq_ex->status;
                    wc.wr_id = ctx->cq_s.cq_ex->wr_id;
                    wc.byte_len = ibv_wc_read_byte_len(ctx->cq_s.cq_ex);
                    wc.opcode = ctx->cq_s.cq_ex->read_opcode(ctx->cq_s.cq_ex);
                }

                if (wc.status == IBV_WC_SUCCESS) {
                    printf("  [CQE] wc.status: %s(%d)\n",
                           ibv_wc_status_str(wc.status), wc.status);

                    if (wc.opcode == IBV_WC_RECV) {
                        ++cnt_recv;
                        ret = post_recv(ctx, 1);
                        printf("  [CQE] RECV (wr_id: %lu)\n", wc.wr_id);
                        if (cnt_recv == 1) {
                            if (rnic_hw_ts) {
                                ts_client_recv = ts_cqe;
                                printf("Network RTT + Server delay: %lu\n",
                                       ts_client_recv - ts_client_send);
                            }

                            if (gettimeofday(&end, NULL)) {
                                perror("gettimeofday");
                                return 1;
                            }

                            uint32_t usec =
                                (end.tv_sec - start.tv_sec) * 1000000 +
                                (end.tv_usec - start.tv_usec);
                            printf("\tElapsed time: %u microseconds\n", usec);
                        }

                        printf("\tClient received %d bytes. Buffer: %s\n",
                               wc.byte_len, ctx->buf + grh_size);

                        if (cnt_recv == 2) {
                            printf("\tThis is the last ACK\n");

                            if (rnic_hw_ts) {
                                auto server_delay = std::strtoull(
                                    ctx->buf + grh_size, nullptr, 10);
                                printf("\tServer delay: %llu\n", server_delay);
                            }
                            printf("\tClient completed.\n");
                            exit(1);
                        }
                    }

                    if (wc.opcode == IBV_WC_SEND) {
                        if (rnic_hw_ts) {
                            ts_client_send = ts_cqe;
                        }
                        printf("  [CQE] SEND (wr_id: %lu)\n", wc.wr_id);
                    }

                } else {
                    perror("Failed to get IBV_WC_SUCCESS\n");
                    exit(1);
                }
            }
        }
    }

    delete (ctx);
}
