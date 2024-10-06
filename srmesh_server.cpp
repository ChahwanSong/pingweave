#include <arpa/inet.h>
#include <ifaddrs.h>
#include <infiniband/verbs.h>
#include <netdb.h>
#include <unistd.h>
#include <yaml-cpp/yaml.h>

#include <atomic>
#include <chrono>
#include <cstring>
#include <fstream>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

constexpr int QKEY = 0x11111111;  // QKEY 상수 값, QP의 QKEY 설정
constexpr int BUFFER_SIZE = 50;  // 버퍼 크기 상수 값, 수신/송신할 메시지 크기

std::atomic<bool> stop_flag(false);  // 서버 종료 플래그
std::mutex print_mutex;              // 출력 보호 mutex
std::mutex file_mutex;               // 파일 접근 보호 mutex

// 작업 완료 상태에 따른 오류 메시지 출력 함수
void print_wc_error_message(ibv_wc_status status) {
    // 자세한 오류 메시지 (IBV_WC_* 상수값으로 매핑 가능)
    switch (status) {
        case IBV_WC_LOC_LEN_ERR:
            std::cerr << "Error: Local length error." << std::endl;
            break;
        case IBV_WC_LOC_QP_OP_ERR:
            std::cerr << "Error: Local QP operation error." << std::endl;
            break;
        case IBV_WC_LOC_PROT_ERR:
            std::cerr << "Error: Local protection error." << std::endl;
            break;
        case IBV_WC_WR_FLUSH_ERR:
            std::cerr << "Error: Work request flushed error." << std::endl;
            break;
        case IBV_WC_MW_BIND_ERR:
            std::cerr << "Error: Memory window bind error." << std::endl;
            break;
        case IBV_WC_BAD_RESP_ERR:
            std::cerr << "Error: Bad response error." << std::endl;
            break;
        case IBV_WC_LOC_ACCESS_ERR:
            std::cerr << "Error: Local access error." << std::endl;
            break;
        case IBV_WC_REM_INV_REQ_ERR:
            std::cerr << "Error: Remote invalid request error." << std::endl;
            break;
        case IBV_WC_REM_ACCESS_ERR:
            std::cerr << "Error: Remote access error." << std::endl;
            break;
        case IBV_WC_REM_OP_ERR:
            std::cerr << "Error: Remote operation error." << std::endl;
            break;
        case IBV_WC_RETRY_EXC_ERR:
            std::cerr << "Error: Retry exceeded error." << std::endl;
            break;
        case IBV_WC_RNR_RETRY_EXC_ERR:
            std::cerr << "Error: Receiver not ready (RNR) retry exceeded error."
                      << std::endl;
            break;
        case IBV_WC_REM_ABORT_ERR:
            std::cerr << "Error: Remote aborted error." << std::endl;
            break;
        default:
            std::cerr << "Error: Unknown work completion error." << std::endl;
            break;
    }
}

// pinglist.yaml 파일에서 RDMA IP 리스트를 가져오는 함수
std::vector<std::string> get_rdma_ips_from_pinglist(
    const std::string& file_path) {
    std::vector<std::string> rdma_ips;
    YAML::Node pinglist = YAML::LoadFile(file_path);

    // pinglist의 RDMA 카테고리에서 IP 리스트를 가져옴
    for (const auto& group : pinglist["rdma"]) {
        for (const auto& ip : group.second) {
            rdma_ips.push_back(ip.as<std::string>());
        }
    }

    return rdma_ips;
}

// RDMA 리소스 해제 함수
void cleanup_rdma_resources(struct ibv_cq* completion_queue,
                            struct ibv_qp* queue_pair,
                            struct ibv_mr* memory_region,
                            struct ibv_comp_channel* comp_channel,
                            struct ibv_pd* pd, struct ibv_context* context) {
    // 각 리소스를 안전하게 해제
    if (queue_pair) {
        if (ibv_destroy_qp(queue_pair)) {
            std::cerr << "Error: Failed to destroy queue pair." << std::endl;
        }
    }

    if (memory_region) {
        if (ibv_dereg_mr(memory_region)) {
            std::cerr << "Error: Failed to deregister memory region."
                      << std::endl;
        }
    }

    if (completion_queue) {
        if (ibv_destroy_cq(completion_queue)) {
            std::cerr << "Error: Failed to destroy completion queue."
                      << std::endl;
        }
    }

    if (comp_channel) {
        if (ibv_destroy_comp_channel(comp_channel)) {
            std::cerr << "Error: Failed to destroy completion channel."
                      << std::endl;
        }
    }

    if (pd) {
        if (ibv_dealloc_pd(pd)) {
            std::cerr << "Error: Failed to deallocate protection domain."
                      << std::endl;
        }
    }

    if (context) {
        if (ibv_close_device(context)) {
            std::cerr << "Error: Failed to close RDMA device context."
                      << std::endl;
        }
    }
}

// RDMA 리소스를 설정하는 함수
bool setup_rdma_resources(struct ibv_context* context, struct ibv_pd*& pd,
                          struct ibv_cq*& completion_queue,
                          struct ibv_qp*& queue_pair,
                          struct ibv_comp_channel*& comp_channel) {
    // Protection Domain 생성
    pd = ibv_alloc_pd(context);
    if (!pd) {
        std::cerr << "Error: Failed to allocate protection domain."
                  << std::endl;
        return false;
    }

    // Completion Channel 생성
    comp_channel = ibv_create_comp_channel(context);
    if (!comp_channel) {
        std::cerr << "Error: Failed to create completion channel." << std::endl;
        ibv_dealloc_pd(pd);  // 리소스 해제
        return false;
    }

    // Completion Queue 생성
    completion_queue = ibv_create_cq(context, 16, nullptr, comp_channel, 0);
    if (!completion_queue) {
        std::cerr << "Error: Failed to create completion queue." << std::endl;
        ibv_destroy_comp_channel(comp_channel);
        ibv_dealloc_pd(pd);
        return false;
    }

    // CQ 이벤트 알림 요청
    if (ibv_req_notify_cq(completion_queue, 0)) {
        std::cerr << "Error: Failed to request CQ notification." << std::endl;
        ibv_destroy_cq(completion_queue);
        ibv_destroy_comp_channel(comp_channel);
        ibv_dealloc_pd(pd);
        return false;
    }

    // Queue Pair(QP) 초기화
    struct ibv_qp_init_attr qp_init_attr = {};
    qp_init_attr.send_cq = completion_queue;  // 송신 및 수신 CQ 연결
    qp_init_attr.recv_cq = completion_queue;
    qp_init_attr.cap.max_send_wr = 16;  // 최대 송신 작업 요청
    qp_init_attr.cap.max_recv_wr = 16;  // 최대 수신 작업 요청
    qp_init_attr.cap.max_send_sge = 1;  // 최대 송신 SGE
    qp_init_attr.cap.max_recv_sge = 1;  // 최대 수신 SGE
    qp_init_attr.qp_type = IBV_QPT_UD;  // QP 유형: Unreliable Datagram (UD)

    queue_pair = ibv_create_qp(pd, &qp_init_attr);  // QP 생성
    if (!queue_pair) {
        std::cerr << "Error: Failed to create queue pair." << std::endl;
        ibv_destroy_cq(completion_queue);
        ibv_destroy_comp_channel(comp_channel);
        ibv_dealloc_pd(pd);
        return false;
    }

    return true;
}

// QP의 상태를 INIT -> RTR -> RTS로 변경하는 함수
bool modify_qp_to_rtr_rts(struct ibv_qp* queue_pair, int port_num) {
    struct ibv_qp_attr qp_attr = {};

    // INIT 상태로 변경
    qp_attr.qp_state = IBV_QPS_INIT;
    qp_attr.pkey_index = 0;
    qp_attr.port_num = port_num;
    qp_attr.qkey = QKEY;

    if (ibv_modify_qp(
            queue_pair, &qp_attr,
            IBV_QP_STATE | IBV_QP_PKEY_INDEX | IBV_QP_PORT | IBV_QP_QKEY)) {
        std::cerr << "Error: Failed to modify QP to INIT state." << std::endl;
        return false;
    }

    // RTR 상태로 변경
    qp_attr.qp_state = IBV_QPS_RTR;
    if (ibv_modify_qp(queue_pair, &qp_attr, IBV_QP_STATE)) {
        std::cerr << "Error: Failed to transition QP to RTR state."
                  << std::endl;
        return false;
    }

    // RTS 상태로 변경
    qp_attr.qp_state = IBV_QPS_RTS;
    qp_attr.sq_psn = 0;  // 송신 PSN 초기화
    if (ibv_modify_qp(queue_pair, &qp_attr, IBV_QP_STATE | IBV_QP_SQ_PSN)) {
        std::cerr << "Error: Failed to transition QP to RTS state."
                  << std::endl;
        return false;
    }

    return true;
}

// RDMA 이벤트 처리 함수
void handle_rdma_events(struct ibv_cq* completion_queue,
                        struct ibv_qp* queue_pair, char* buffer,
                        struct ibv_mr* memory_region,
                        struct ibv_comp_channel* comp_channel) {
    struct ibv_sge sge = {};  // Scatter/Gather Element 설정
    sge.addr = (uintptr_t)buffer;
    sge.length = BUFFER_SIZE;
    sge.lkey = memory_region->lkey;

    struct ibv_recv_wr recv_wr = {};  // 수신 작업 요청 구조체
    struct ibv_recv_wr* bad_recv_wr = nullptr;
    recv_wr.sg_list = &sge;  // 수신 데이터 설정
    recv_wr.num_sge = 1;

    // 수신 요청 게시
    if (ibv_post_recv(queue_pair, &recv_wr, &bad_recv_wr)) {
        std::cerr << "Error: Failed to post receive request." << std::endl;
        return;
    }

    // 서버가 중단될 때까지 이벤트 처리 반복
    while (!stop_flag) {
        struct ibv_cq* ev_cq;
        void* cq_context;

        // CQ 이벤트를 대기하고 가져옴
        if (ibv_get_cq_event(comp_channel, &ev_cq, &cq_context)) {
            std::cerr << "Error: Failed to get CQ event." << std::endl;
            break;
        }

        // CQ 이벤트 acknowledge
        ibv_ack_cq_events(completion_queue, 1);

        // 다음 CQ 이벤트 알림 요청
        if (ibv_req_notify_cq(completion_queue, 0)) {
            std::cerr << "Error: Failed to request CQ notification."
                      << std::endl;
            break;
        }

        struct ibv_wc work_completion;  // 작업 완료 구조체
        int num_completions = ibv_poll_cq(
            completion_queue, 1, &work_completion);  // CQ에서 작업 완료 폴링

        // 작업 완료 시 메시지 처리
        if (num_completions > 0) {
            if (work_completion.status == IBV_WC_SUCCESS) {
                std::cout << "Received message: " << buffer << std::endl;

                // 응답 메시지 준비
                strcpy(buffer, "Hello from Server");

                struct ibv_send_wr send_wr = {};  // 송신 작업 요청
                struct ibv_send_wr* bad_send_wr = nullptr;
                send_wr.sg_list = &sge;  // 송신 데이터 설정
                send_wr.num_sge = 1;
                send_wr.opcode = IBV_WR_SEND;            // 송신 작업 요청
                send_wr.send_flags = IBV_SEND_SIGNALED;  // 작업 완료 시 신호

                // 송신 요청 게시
                if (ibv_post_send(queue_pair, &send_wr, &bad_send_wr)) {
                    std::cerr << "Error: Failed to post send request."
                              << std::endl;
                    break;
                }

                // 수신 요청을 다시 게시 (계속 수신 대기)
                if (ibv_post_recv(queue_pair, &recv_wr, &bad_recv_wr)) {
                    std::cerr << "Error: Failed to post receive request."
                              << std::endl;
                    break;
                }
            } else {
                // 작업 완료 오류 발생 시 메시지 출력
                print_wc_error_message(work_completion.status);
                break;  // 오류 발생 시 루프 탈출
            }
        }
    }
}

// RDMA 서버 정보 출력 후 파일 저장 함수
void save_server_info(const std::string& rdma_ip, const std::string& gid_str,
                      uint32_t qp_num) {
    std::lock_guard<std::mutex> guard(file_mutex);  // 파일 접근 보호

    // CSV 파일에 정보를 저장
    std::ofstream outfile("config/local_server_info.csv", std::ios::app);
    if (outfile.is_open()) {
        outfile << rdma_ip << "," << gid_str << "," << qp_num << std::endl;
        outfile.close();
    } else {
        std::cerr << "Error: Failed to open file for writing." << std::endl;
    }
}

// 서버 실행 함수
void run_server(struct ibv_context* context, int port_num,
                const std::string& rdma_ip) {
    struct ibv_pd* pd = nullptr;
    struct ibv_cq* completion_queue = nullptr;
    struct ibv_qp* queue_pair = nullptr;
    struct ibv_mr* memory_region = nullptr;
    struct ibv_comp_channel* comp_channel = nullptr;
    char buffer[BUFFER_SIZE] = {0};  // 메시지를 담을 버퍼

    try {
        // RDMA 리소스 설정
        if (!setup_rdma_resources(context, pd, completion_queue, queue_pair,
                                  comp_channel)) {
            throw std::runtime_error("Failed to setup RDMA resources.");
        }

        // QP 상태 변경
        if (!modify_qp_to_rtr_rts(queue_pair, port_num)) {
            throw std::runtime_error("Failed to modify QP state.");
        }

        // 메모리 등록
        memory_region =
            ibv_reg_mr(pd, buffer, sizeof(buffer), IBV_ACCESS_LOCAL_WRITE);
        if (!memory_region) {
            throw std::runtime_error("Failed to register memory region.");
        }

        // GID (Global Identifier) 조회
        union ibv_gid gid;
        if (ibv_query_gid(context, port_num, 0, &gid)) {
            throw std::runtime_error("Failed to query GID.");
        }

        char gid_str[INET6_ADDRSTRLEN];
        inet_ntop(AF_INET6, &gid, gid_str,
                  sizeof(gid_str));  // GID를 문자열로 변환

        // 서버 정보 출력
        {
            std::lock_guard<std::mutex> guard(print_mutex);  // 출력 보호
            std::cout << "Server on RDMA IP " << rdma_ip << " has started."
                      << std::endl;
            std::cout << "Server GID: " << gid_str << std::endl;
            std::cout << "Server QPN: " << queue_pair->qp_num << std::endl;
        }

        // 서버 정보를 CSV 파일에 저장
        save_server_info(rdma_ip, gid_str, queue_pair->qp_num);

        // RDMA 이벤트 처리 루프 시작
        handle_rdma_events(completion_queue, queue_pair, buffer, memory_region,
                           comp_channel);

    } catch (const std::exception& e) {
        // 예외 발생 시 에러 메시지 출력 및 서버 종료
        std::lock_guard<std::mutex> guard(print_mutex);  // 출력 보호
        std::cerr << "Server on RDMA IP " << rdma_ip
                  << " encountered an error: " << e.what() << std::endl;
        stop_flag = true;
    }

    // RDMA 리소스 해제
    cleanup_rdma_resources(completion_queue, queue_pair, memory_region,
                           comp_channel, pd, context);
    std::lock_guard<std::mutex> guard(print_mutex);  // 출력 보호
    std::cout << "Server on RDMA IP " << rdma_ip << " has stopped."
              << std::endl;
    stop_flag = true;
}

// RDMA 장치가 유효한지 확인하는 함수
bool is_device_valid(struct ibv_context* context, int port_num) {
    struct ibv_port_attr port_attr;
    if (ibv_query_port(context, port_num, &port_attr) == 0) {
        return port_attr.state == IBV_PORT_ACTIVE;  // 포트가 활성 상태인지 확인
    }
    return false;
}

// RDMA 장치의 IP가 특정 IP와 일치하는지 확인하는 함수
bool match_device_to_ip(struct ibv_context* context, const std::string& ip,
                        int& matched_port) {
    struct in_addr addr;
    if (inet_pton(AF_INET, ip.c_str(), &addr) != 1) {  // IP 주소를 한번만 변환
        std::cerr << "Error: Invalid IP address format for " << ip << std::endl;
        return false;
    }

    for (int port_num = 1; port_num <= 2; ++port_num) {
        union ibv_gid gid;
        if (ibv_query_gid(context, port_num, 0, &gid) == 0) {
            // GID가 IPv4 기반인지 확인 (IPv6에서 GID의 마지막 4바이트가 IPv4
            // 주소로 사용됨)
            uint32_t gid_ipv4 = *(reinterpret_cast<uint32_t*>(&gid.raw[12]));

            if (gid_ipv4 == addr.s_addr) {  // GID에서 추출한 IPv4와 비교
                if (is_device_valid(context, port_num)) {
                    matched_port = port_num;
                    return true;  // 유효한 장치 발견
                }
            }
        } else {
            std::cerr << "Error: Failed to query GID for port " << port_num
                      << std::endl;
        }
    }
    return false;
}

// 로컬 IP 주소를 가져오는 함수
std::vector<std::string> get_local_ip_addresses() {
    std::vector<std::string> local_ips;
    struct ifaddrs* ifaddr;
    if (getifaddrs(&ifaddr) == -1) {  // 인터페이스 주소 정보 가져오기
        perror("getifaddrs");
        return local_ips;
    }

    // 인터페이스를 순회하며 IPv4 주소를 찾음
    for (struct ifaddrs* ifa = ifaddr; ifa != nullptr; ifa = ifa->ifa_next) {
        if (ifa->ifa_addr == nullptr) continue;
        if (ifa->ifa_addr->sa_family == AF_INET) {  // IPv4 주소 확인
            char host[NI_MAXHOST];
            if (getnameinfo(ifa->ifa_addr, sizeof(struct sockaddr_in), host,
                            NI_MAXHOST, nullptr, 0, NI_NUMERICHOST) == 0) {
                std::cout << "Local address is detected: " << host << std::endl;
                local_ips.push_back(host);  // 로컬 IP 주소 추가
            }
        }
    }

    freeifaddrs(ifaddr);  // 메모리 해제
    return local_ips;
}

int main() {
    // 로컬 IP 주소 가져오기
    std::vector<std::string> local_ips = get_local_ip_addresses();

    // pinglist.yaml 파일에서 RDMA IP 리스트 가져오기
    std::vector<std::string> rdma_ips =
        get_rdma_ips_from_pinglist("config/pinglist.yaml");

    // RDMA 장치 리스트 가져오기
    int num_devices;
    struct ibv_device** device_list = ibv_get_device_list(&num_devices);
    if (!device_list) {
        std::cerr << "Error: Failed to get RDMA devices." << std::endl;
        return 1;
    }

    std::vector<std::pair<std::thread, struct ibv_context*>> server_threads;

    // 각 RDMA 장치에 대해 RDMA IP와 일치하는 포트를 찾고 서버 실행
    for (int i = 0; i < num_devices; ++i) {
        struct ibv_device* dev = device_list[i];
        struct ibv_context* context = ibv_open_device(dev);
        if (!context) continue;

        for (const std::string& rdma_ip : rdma_ips) {
            int matched_port = 0;
            // 매칭되는 포트가 있으면 서버 실행
            if (match_device_to_ip(context, rdma_ip, matched_port)) {
                // 서버 실행 스레드 추가
                server_threads.emplace_back(std::make_pair(
                    std::thread(run_server, context, matched_port, rdma_ip),
                    context));
            }
        }
    }

    // 모든 서버 스레드가 종료될 때까지 대기
    for (auto& [thread, context] : server_threads) {
        if (thread.joinable()) {
            thread.join();
        }
        // RDMA 장치 닫기
        ibv_close_device(context);
    }

    // RDMA 장치 리스트 해제
    ibv_free_device_list(device_list);
    return 0;
}
