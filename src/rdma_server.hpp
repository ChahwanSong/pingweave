#pragma once

#include "rdma_common.hpp"

void rdma_server(const std::string& ip_addr) {
    // init logger
    const std::string logname = "rdma_server_" + ip_addr;
    auto logger = initialize_custom_logger(logname, LOG_LEVEL_SERVER);

    // load a device and init rdma ctx
    struct pingweave_context ctx_rx;
    struct pingweave_dest my_dest_rx;
    struct pingweave_dest rem_dest_rx;
    int ret;
    char parsed_gid[33];
    char wired_gid[33];

    ctx_rx.is_rx = true;
    ctx_rx.context = get_context_by_ip(ip_addr.c_str());
    logger->info("Device: {}", ctx_rx.context->device->name);

    // initialize context
    if (init_ctx(&ctx_rx)) { /* failed */
        logger->error("Couldn't initialize context");
        printf("Couldn't initialize context");
        exit(1);
    }

    if (connect_ctx(&ctx_rx)) {
        logger->error("Couldn't modify QP state");
        exit(1);
    }

    if (ibv_req_notify_cq(pingweave_cq(&ctx_rx), 0)) {
        logger->error("Couldn't request CQ notification");
        exit(1);
    }

    my_dest_rx.lid = ctx_rx.portinfo.lid;
    my_dest_rx.qpn = ctx_rx.qp->qp_num;
    if (ibv_query_gid(ctx_rx.context, ctx_rx.active_port, GID_INDEX,
                      &my_dest_rx.gid)) {
        logger->error("Could not get local gid for gid index {}", GID_INDEX);
        exit(1);
    }

    inet_ntop(AF_INET6, &my_dest_rx.gid, parsed_gid, sizeof parsed_gid);
    gid_to_wire_gid(&my_dest_rx.gid, wired_gid);

    logger->info("local info:  IP {}, LID {}, QPN {}, GID {}", ip_addr,
                 my_dest_rx.lid, my_dest_rx.qpn, parsed_gid);

    // record server information (ip, lid, gid, qpn) as file

    // while loop...

    while (true) {
        logger->info("Waiting 1 seconds...");
        sleep(1);
    }
}