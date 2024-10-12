#include "common.hpp"

int init_ctx(struct pingpong_context *ctx) {
    page_size = sysconf(_SC_PAGESIZE);
    printf("Page size: %d\n", page_size);

    ctx->send_flags = IBV_SEND_SIGNALED;
    ctx->rx_depth = rx_depth;

    if (!posix_memalign((void **)&ctx->buf, page_size, msg_size + grh_size)) {
        std::cerr << "ctx->buf memalign failed\n" << std::endl;
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
        ctx->cq =
            ibv_create_cq(ctx->context, rx_depth + 1, NULL, ctx->channel, 0);
        if (!ctx->cq) {
            fprintf(stderr, "Couldn't create CQ\n");
            goto clean_mr;
        }
    }

    {
        struct ibv_qp_attr attr;
        struct ibv_qp_init_attr init_attr = {};
        init_attr.send_cq = ctx->cq;
        init_attr.recv_cq = ctx->cq;
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
    ibv_destroy_cq(ctx->cq);

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

static int connect_ctx(struct pingpong_context *ctx, int my_psn,
                       struct pingpong_dest *rem_dest) {
    struct ibv_ah_attr ah_attr = {};
    ah_attr.is_global = 0;
    ah_attr.dlid = rem_dest->lid;
    ah_attr.sl = service_level;
    ah_attr.src_path_bits = 0;
    ah_attr.port_num = ctx->active_port;

    struct ibv_qp_attr attr = {};
    attr.qp_state = IBV_QPS_RTR;
    if (ibv_modify_qp(ctx->qp, &attr, IBV_QP_STATE)) {
        fprintf(stderr, "Failed to modify QP to RTR\n");
        return 1;
    }

    attr.qp_state = IBV_QPS_RTS;
    attr.sq_psn = my_psn;

    if (ibv_modify_qp(ctx->qp, &attr, IBV_QP_STATE | IBV_QP_SQ_PSN)) {
        fprintf(stderr, "Failed to modify QP to RTS\n");
        return 1;
    }

    if (rem_dest->gid.global.interface_id) {
        ah_attr.is_global = 1;
        ah_attr.grh.hop_limit = 1;
        ah_attr.grh.dgid = rem_dest->gid;
        ah_attr.grh.sgid_index = gid_index;
    }

    ctx->ah = ibv_create_ah(ctx->pd, &ah_attr);
    if (!ctx->ah) {
        fprintf(stderr, "Failed to create AH\n");
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

static int post_send(struct pingpong_context *ctx, uint32_t qpn,
                     std::string msg) {
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
    wr.wr.ud.remote_qpn = qpn;
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

    // Request CQ Notification
    if (use_event) {
        if (ibv_req_notify_cq(ctx->cq, 0)) {
            fprintf(stderr, "Couldn't request CQ notification\n");
            exit(1);
        }
    }

    my_dest.lid = ctx->portinfo.lid;
    my_dest.qpn = ctx->qp->qp_num;
    my_dest.psn = lrand48() & 0xffffff;
    if (ibv_query_gid(ctx->context, ctx->active_port, gid_index,
                      &my_dest.gid)) {
        fprintf(stderr, "Could not get local gid for gid index %d\n",
                gid_index);
        exit(1);
    }

    inet_ntop(AF_INET6, &my_dest.gid, parsed_gid, sizeof parsed_gid);
    printf("  local address:  IP %s, LID %d, QPN %d, PSN %d: GID %s\n",
           target_ip, my_dest.lid, my_dest.qpn, my_dest.psn, parsed_gid);

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
    if (connect_ctx(ctx, my_dest.psn, &rem_dest)) {
        fprintf(stderr, "Couldn't modify QP state\n");
        exit(1);
    }

    // RECV WR 포스트
    int num_init_post = 1;
    ret = post_recv(ctx, num_init_post);
    if (ret < num_init_post) {
        fprintf(stderr, "Failed to post all RECV (%d/%d)\n", ret,
                num_init_post);
    }

    if (ctx->is_server) {  // 서버
        int cnt_send = 0;
        struct ibv_wc wc = {};
        while (true) {
            if (use_event) {
                struct ibv_cq *ev_cq;
                void *ev_ctx;
                if (ibv_get_cq_event(ctx->channel, &ev_cq, &ev_ctx)) {
                    fprintf(stderr, "Failed to get cq_event\n");
                    return 1;
                }

                ibv_ack_cq_events(ctx->cq, 1);

                if (ev_cq != ctx->cq) {
                    fprintf(stderr, "CQ event for unknown CQ %p\n", ev_cq);
                    return 1;
                }
                if (ibv_req_notify_cq(ctx->cq, 0)) {
                    fprintf(stderr, "Couldn't request CQ notification\n");
                    return 1;
                }
            }

            int num_cqe = ibv_poll_cq(ctx->cq, 1, &wc);
            if (num_cqe < 0) {
                std::cerr << "Poll CQ failed" << std::endl;
                break;
            } else if (num_cqe > 1) {
                std::cerr << "CQE must be 1" << std::endl;
                break;
            }

            if (num_cqe > 0) {
                printf("* CQE Event happend! (%d)\n", num_cqe);

                if (wc.status == IBV_WC_SUCCESS) {
                    printf("  [CQE] wc.status: %d\n", wc.status);

                    if (wc.opcode == IBV_WC_RECV) {
                        ret = post_recv(ctx, 1);  // 다시 수신 대기
                        printf("  [CQE] RECV (wr_id: %lu)\n", wc.wr_id);
                        printf("Server received %d bytes. Buffer: %s\n",
                               wc.byte_len, ctx->buf + grh_size);

                        /* Post a send with a message */
                        if (post_send(ctx, rem_dest.qpn,
                                      msg_from_server + std::string("_1"))) {
                            fprintf(stderr, "Couldn't post send\n");
                            exit(1);
                        }

                        /* Post a send with a message */
                        usleep(1000);
                        if (post_send(ctx, rem_dest.qpn,
                                      msg_from_server + std::string("_2"))) {
                            fprintf(stderr, "Couldn't post send\n");
                            exit(1);
                        }
                    }

                    if (wc.opcode == IBV_WC_SEND) {
                        printf("  [CQE] SEND (wr_id: %lu)\n", wc.wr_id);
                        ++cnt_send;
                        if (cnt_send >= 2) {
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

    } else {               // 클라이언트
        int cnt_recv = 0;  // expect 2 RECVs
        struct ibv_wc wc = {};
        if (gettimeofday(&start, NULL)) {
            perror("gettimeofday");
            return 1;
        }

        // 일단 클라이언트 먼저 message 보내기
        if (post_send(ctx, rem_dest.qpn, msg_from_client)) {
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

                ibv_ack_cq_events(ctx->cq, 1);

                if (ev_cq != ctx->cq) {
                    fprintf(stderr, "CQ event for unknown CQ %p\n", ev_cq);
                    return 1;
                }
                if (ibv_req_notify_cq(ctx->cq, 0)) {
                    fprintf(stderr, "Couldn't request CQ notification\n");
                    return 1;
                }
            }

            int num_cqe = ibv_poll_cq(ctx->cq, 1, &wc);
            if (num_cqe < 0) {
                std::cerr << "Poll CQ failed" << std::endl;
                break;
            } else if (num_cqe > 1) {
                std::cerr << "CQE must be 1" << std::endl;
                break;
            }

            if (num_cqe > 0) {
                printf("* CQE Event happend! (%d)\n", num_cqe);

                if (wc.status == IBV_WC_SUCCESS) {
                    printf("  [CQE] wc.status: %d\n", wc.status);

                    if (wc.opcode == IBV_WC_RECV) {
                        ++cnt_recv;
                        ret = post_recv(ctx, 1);
                        printf("  [CQE] RECV (wr_id: %lu)\n", wc.wr_id);
                        if (cnt_recv == 1) {
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
                            printf("Client completed.\n");
                            exit(1);
                        }
                    }

                    if (wc.opcode == IBV_WC_SEND) {
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
