#include <infiniband/verbs.h>

#include <chrono>
#include <cstring>
#include <iostream>

constexpr int PORT_NUM = 1;
constexpr int QKEY = 0x11111111;

void cleanup_resources(struct ibv_qp* qp, struct ibv_cq* cq, struct ibv_pd* pd,
                       struct ibv_context* context, struct ibv_mr* mr,
                       struct ibv_ah* ah) {
    if (ah) ibv_destroy_ah(ah);
    if (mr) ibv_dereg_mr(mr);
    if (qp) ibv_destroy_qp(qp);
    if (cq) ibv_destroy_cq(cq);
    if (pd) ibv_dealloc_pd(pd);
    if (context) ibv_close_device(context);
}

void run_client(union ibv_gid server_gid, uint32_t server_qp_number) {
    struct ibv_context* context = nullptr;
    struct ibv_pd* protection_domain = nullptr;
    struct ibv_cq* completion_queue = nullptr;
    struct ibv_qp* queue_pair = nullptr;
    struct ibv_mr* memory_region = nullptr;
    struct ibv_ah* address_handle = nullptr;
    struct ibv_wc work_completion;
    char buffer[1024] = {0};

    // InfiniBand 장치 열기
    context = ibv_open_device(nullptr);
    if (!context) {
        std::cerr << "Error: Failed to open InfiniBand device." << std::endl;
        return;
    }

    // 보호 도메인 생성
    protection_domain = ibv_alloc_pd(context);
    if (!protection_domain) {
        std::cerr << "Error: Failed to allocate protection domain."
                  << std::endl;
        cleanup_resources(queue_pair, completion_queue, protection_domain,
                          context, memory_region, address_handle);
        return;
    }

    // 완료 큐 생성
    completion_queue = ibv_create_cq(context, 16, nullptr, nullptr, 0);
    if (!completion_queue) {
        std::cerr << "Error: Failed to create completion queue." << std::endl;
        cleanup_resources(queue_pair, completion_queue, protection_domain,
                          context, memory_region, address_handle);
        return;
    }

    // QP 설정
    struct ibv_qp_init_attr qp_init_attr = {};
    qp_init_attr.send_cq = completion_queue;
    qp_init_attr.recv_cq = completion_queue;
    qp_init_attr.cap.max_send_wr = 16;
    qp_init_attr.cap.max_recv_wr = 16;
    qp_init_attr.cap.max_send_sge = 1;
    qp_init_attr.cap.max_recv_sge = 1;
    qp_init_attr.qp_type = IBV_QPT_UD;

    queue_pair = ibv_create_qp(protection_domain, &qp_init_attr);
    if (!queue_pair) {
        std::cerr << "Error: Failed to create queue pair." << std::endl;
        cleanup_resources(queue_pair, completion_queue, protection_domain,
                          context, memory_region, address_handle);
        return;
    }

    // QP INIT 상태로 전환
    struct ibv_qp_attr qp_attr = {};
    qp_attr.qp_state = IBV_QPS_INIT;
    qp_attr.pkey_index = 0;
    qp_attr.port_num = PORT_NUM;
    qp_attr.qkey = QKEY;

    if (ibv_modify_qp(
            queue_pair, &qp_attr,
            IBV_QP_STATE | IBV_QP_PKEY_INDEX | IBV_QP_PORT | IBV_QP_QKEY)) {
        std::cerr << "Error: Failed to modify QP to INIT state." << std::endl;
        cleanup_resources(queue_pair, completion_queue, protection_domain,
                          context, memory_region, address_handle);
        return;
    }

    // QP 상태 변경 (RTR 및 RTS 상태로 전환)
    qp_attr.qp_state = IBV_QPS_RTR;
    if (ibv_modify_qp(queue_pair, &qp_attr, IBV_QP_STATE)) {
        std::cerr << "Error: Failed to transition QP to RTR state."
                  << std::endl;
        cleanup_resources(queue_pair, completion_queue, protection_domain,
                          context, memory_region, address_handle);
        return;
    }

    qp_attr.qp_state = IBV_QPS_RTS;
    qp_attr.sq_psn = 0;
    if (ibv_modify_qp(queue_pair, &qp_attr, IBV_QP_STATE | IBV_QP_SQ_PSN)) {
        std::cerr << "Error: Failed to transition QP to RTS state."
                  << std::endl;
        cleanup_resources(queue_pair, completion_queue, protection_domain,
                          context, memory_region, address_handle);
        return;
    }

    // Memory Region 설정
    memory_region = ibv_reg_mr(protection_domain, buffer, sizeof(buffer),
                               IBV_ACCESS_LOCAL_WRITE);
    if (!memory_region) {
        std::cerr << "Error: Failed to register memory region." << std::endl;
        cleanup_resources(queue_pair, completion_queue, protection_domain,
                          context, memory_region, address_handle);
        return;
    }

    // 주소 정보 설정
    struct ibv_ah_attr ah_attr = {};
    ah_attr.is_global = 1;
    ah_attr.grh.dgid = server_gid;
    ah_attr.grh.sgid_index = 0;
    ah_attr.port_num = PORT_NUM;

    address_handle = ibv_create_ah(protection_domain, &ah_attr);
    if (!address_handle) {
        std::cerr << "Error: Failed to create address handle." << std::endl;
        cleanup_resources(queue_pair, completion_queue, protection_domain,
                          context, memory_region, address_handle);
        return;
    }

    // 메시지 전송
    std::strcpy(buffer, "Hello from Client");

    struct ibv_sge sge = {};
    sge.addr = (uintptr_t)buffer;
    sge.length = sizeof(buffer);
    sge.lkey = memory_region->lkey;

    struct ibv_send_wr send_wr = {};
    struct ibv_send_wr* bad_send_wr = nullptr;
    send_wr.wr_id = 0;
    send_wr.opcode = IBV_WR_SEND;
    send_wr.sg_list = &sge;
    send_wr.num_sge = 1;
    send_wr.send_flags = IBV_SEND_SIGNALED;
    send_wr.wr.ud.ah = address_handle;
    send_wr.wr.ud.remote_qpn = server_qp_number;
    send_wr.wr.ud.remote_qkey = QKEY;

    // 시간 측정 시작
    auto start_time = std::chrono::steady_clock::now();

    if (ibv_post_send(queue_pair, &send_wr, &bad_send_wr)) {
        std::cerr << "Error: Failed to post send request." << std::endl;
        cleanup_resources(queue_pair, completion_queue, protection_domain,
                          context, memory_region, address_handle);
        return;
    }

    // 응답 메시지 수신 대기 - 타임아웃 설정
    bool received_response = false;
    while (true) {
        // 완료 큐에서 응답 확인
        int num_completions =
            ibv_poll_cq(completion_queue, 1, &work_completion);

        if (num_completions > 0 && work_completion.status == IBV_WC_SUCCESS) {
            // 시간 측정 완료
            auto end_time = std::chrono::steady_clock::now();
            std::chrono::duration<double, std::milli> elapsed_time =
                end_time - start_time;

            std::cout << "Received response: " << buffer << std::endl;
            std::cout << "Time taken for message round-trip: "
                      << elapsed_time.count() << " ms" << std::endl;
            received_response = true;
            break;
        }

        // 타임아웃 처리 (500ms 초과 시)
        auto current_time = std::chrono::steady_clock::now();
        auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(
            current_time - start_time);
        if (duration.count() > 500) {
            std::cout
                << "Timeout: Server did not respond within 500 milliseconds."
                << std::endl;
            break;
        }
    }

    if (!received_response) {
        std::cout << "No response received from server." << std::endl;
    }

    // 자원 해제
    cleanup_resources(queue_pair, completion_queue, protection_domain, context,
                      memory_region, address_handle);
}

int main() {
    union ibv_gid server_gid;
    uint32_t server_qp_number;

    // 서버 GID 및 QP 번호 설정 (실제 환경에서는 이 값을 적절히 할당)
    // server_gid = ...;
    // server_qp_number = ...;

    run_client(server_gid, server_qp_number);
    return 0;
}
