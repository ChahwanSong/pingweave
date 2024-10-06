#include <arpa/inet.h>
#include <ifaddrs.h>
#include <infiniband/verbs.h>
#include <netdb.h>
#include <yaml-cpp/yaml.h>

#include <chrono>
#include <fstream>
#include <iostream>
#include <sstream>
#include <thread>
#include <vector>

constexpr int QKEY = 0x11111111;  // QP의 QKEY 설정 상수
constexpr int BUFFER_SIZE = 50;   // 송신/수신 버퍼 크기 상수

// 로컬 머신의 IP 주소 목록을 가져오는 함수
std::vector<std::string> get_local_ip_addresses() {
    std::vector<std::string> local_ips;
    struct ifaddrs* ifaddr;
    if (getifaddrs(&ifaddr) == -1) {
        perror("getifaddrs");
        return local_ips;
    }

    // 네트워크 인터페이스를 순회하며 IPv4 주소 수집
    for (struct ifaddrs* ifa = ifaddr; ifa != nullptr; ifa = ifa->ifa_next) {
        if (ifa->ifa_addr == nullptr) continue;
        if (ifa->ifa_addr->sa_family == AF_INET) {
            char host[NI_MAXHOST];
            if (getnameinfo(ifa->ifa_addr, sizeof(struct sockaddr_in), host,
                            NI_MAXHOST, nullptr, 0, NI_NUMERICHOST) == 0) {
                local_ips.push_back(host);
            }
        }
    }

    freeifaddrs(ifaddr);  // 메모리 해제
    return local_ips;
}

// pinglist.yaml 파일에서 RDMA IP 리스트 및 그룹 정보를 가져오는 함수
std::map<std::string, std::vector<std::string>> get_rdma_groups_from_pinglist(
    const std::string& file_path) {
    std::map<std::string, std::vector<std::string>> rdma_groups;
    YAML::Node pinglist = YAML::LoadFile(file_path);

    // pinglist의 RDMA 카테고리에서 그룹별로 IP 리스트를 가져옴
    for (const auto& group : pinglist["rdma"]) {
        std::vector<std::string> ips;
        for (const auto& ip : group.second) {
            ips.push_back(ip.as<std::string>());
        }
        rdma_groups[group.first.as<std::string>()] = ips;
    }

    return rdma_groups;
}

// RDMA 장치의 IP가 특정 IP와 일치하는지 확인하는 함수
bool match_device_to_ip(struct ibv_context* context, const std::string& ip,
                        int& matched_port) {
    struct in_addr addr;
    // 주어진 IP 주소를 `in_addr` 형식으로 변환
    if (inet_pton(AF_INET, ip.c_str(), &addr) != 1) {
        std::cerr << "Error: Invalid IP address format for " << ip << std::endl;
        return false;
    }

    // RDMA 포트 번호를 순회하며 GID와 IP 주소 비교
    for (int port_num = 1; port_num <= 2; ++port_num) {
        union ibv_gid gid;
        if (ibv_query_gid(context, port_num, 0, &gid) == 0) {
            uint32_t gid_ipv4 = *(reinterpret_cast<uint32_t*>(
                &gid.raw[12]));  // GID의 IPv4 부분 추출
            if (gid_ipv4 == addr.s_addr) {
                matched_port = port_num;  // 매칭되는 포트 발견
                return true;
            }
        }
    }
    return false;
}

// RDMA UD 클라이언트 함수
void run_client(struct ibv_context* context, int port_num,
                const std::string& ip,
                const std::vector<std::string>& target_ips) {
    std::cout << "Running client on port " << port_num << " for IP " << ip
              << std::endl;

    // 보호 도메인 할당
    struct ibv_pd* pd = ibv_alloc_pd(context);
    if (!pd) {
        std::cerr << "Error: Failed to allocate protection domain."
                  << std::endl;
        return;
    }

    // 완료 채널과 완료 큐 생성
    struct ibv_comp_channel* comp_channel = ibv_create_comp_channel(context);
    if (!comp_channel) {
        std::cerr << "Error: Failed to create completion channel." << std::endl;
        ibv_dealloc_pd(pd);
        return;
    }

    struct ibv_cq* completion_queue =
        ibv_create_cq(context, 16, nullptr, comp_channel, 0);
    if (!completion_queue) {
        std::cerr << "Error: Failed to create completion queue." << std::endl;
        ibv_destroy_comp_channel(comp_channel);
        ibv_dealloc_pd(pd);
        return;
    }

    // CQ 알림 요청
    if (ibv_req_notify_cq(completion_queue, 0)) {
        std::cerr << "Error: Failed to request CQ notification." << std::endl;
        ibv_destroy_cq(completion_queue);
        ibv_destroy_comp_channel(comp_channel);
        ibv_dealloc_pd(pd);
        return;
    }

    // Queue Pair (QP) 생성
    struct ibv_qp_init_attr qp_init_attr = {};
    qp_init_attr.send_cq = completion_queue;
    qp_init_attr.recv_cq = completion_queue;
    qp_init_attr.cap.max_send_wr = 16;
    qp_init_attr.cap.max_recv_wr = 16;
    qp_init_attr.cap.max_send_sge = 1;
    qp_init_attr.cap.max_recv_sge = 1;
    qp_init_attr.qp_type = IBV_QPT_UD;  // Unreliable Datagram (UD) QP

    struct ibv_qp* queue_pair = ibv_create_qp(pd, &qp_init_attr);  // QP 생성
    if (!queue_pair) {
        std::cerr << "Error: Failed to create queue pair." << std::endl;
        ibv_destroy_cq(completion_queue);
        ibv_destroy_comp_channel(comp_channel);
        ibv_dealloc_pd(pd);
        return;
    }

    // QP 상태 변경: INIT -> RTR -> RTS
    struct ibv_qp_attr qp_attr = {};
    qp_attr.qp_state = IBV_QPS_INIT;
    qp_attr.pkey_index = 0;
    qp_attr.port_num = port_num;
    qp_attr.qkey = QKEY;

    if (ibv_modify_qp(
            queue_pair, &qp_attr,
            IBV_QP_STATE | IBV_QP_PKEY_INDEX | IBV_QP_PORT | IBV_QP_QKEY)) {
        std::cerr << "Error: Failed to modify QP to INIT state." << std::endl;
        ibv_destroy_qp(queue_pair);
        ibv_destroy_cq(completion_queue);
        ibv_destroy_comp_channel(comp_channel);
        ibv_dealloc_pd(pd);
        return;
    }

    qp_attr.qp_state = IBV_QPS_RTR;
    if (ibv_modify_qp(queue_pair, &qp_attr, IBV_QP_STATE)) {
        std::cerr << "Error: Failed to modify QP to RTR state." << std::endl;
        ibv_destroy_qp(queue_pair);
        ibv_destroy_cq(completion_queue);
        ibv_destroy_comp_channel(comp_channel);
        ibv_dealloc_pd(pd);
        return;
    }

    qp_attr.qp_state = IBV_QPS_RTS;
    qp_attr.sq_psn = 0;
    if (ibv_modify_qp(queue_pair, &qp_attr, IBV_QP_STATE | IBV_QP_SQ_PSN)) {
        std::cerr << "Error: Failed to modify QP to RTS state." << std::endl;
        ibv_destroy_qp(queue_pair);
        ibv_destroy_cq(completion_queue);
        ibv_destroy_comp_channel(comp_channel);
        ibv_dealloc_pd(pd);
        return;
    }

    // 메시지 송수신을 위한 버퍼 생성 및 등록
    char buffer[BUFFER_SIZE];
    struct ibv_mr* memory_region =
        ibv_reg_mr(pd, buffer, sizeof(buffer), IBV_ACCESS_LOCAL_WRITE);
    if (!memory_region) {
        std::cerr << "Error: Failed to register memory region." << std::endl;
        ibv_destroy_qp(queue_pair);
        ibv_destroy_cq(completion_queue);
        ibv_destroy_comp_channel(comp_channel);
        ibv_dealloc_pd(pd);
        return;
    }

    // 타겟 IP들에 대해 메시지 송신 및 응답 처리
    for (const auto& target_ip : target_ips) {
        std::cout << "Sending message to target IP: " << target_ip << std::endl;
        // 송신 및 수신 로직을 추가할 수 있음 (RDMA 자원 사용)
    }

    // RDMA 리소스 해제
    ibv_dereg_mr(memory_region);
    ibv_destroy_qp(queue_pair);
    ibv_destroy_cq(completion_queue);
    ibv_destroy_comp_channel(comp_channel);
    ibv_dealloc_pd(pd);
}

int main() {
    // 로컬 IP 주소 가져오기
    std::vector<std::string> local_ips = get_local_ip_addresses();

    // pinglist.yaml 파일에서 RDMA 그룹별 IP 리스트 가져오기
    std::map<std::string, std::vector<std::string>> rdma_groups =
        get_rdma_groups_from_pinglist("config/pinglist.yaml");

    // RDMA 장치 리스트 가져오기
    int num_devices;
    struct ibv_device** device_list = ibv_get_device_list(&num_devices);
    if (!device_list) {
        std::cerr << "Error: Failed to get RDMA devices." << std::endl;
        return 1;
    }

    std::vector<std::pair<std::thread, struct ibv_context*>> client_threads;

    // 각 RDMA 장치에 대해 RDMA IP와 일치하는 포트를 찾고 클라이언트 실행
    for (int i = 0; i < num_devices; ++i) {
        struct ibv_device* dev = device_list[i];
        struct ibv_context* context = ibv_open_device(dev);
        if (!context) continue;  // 장치 열기에 실패하면 건너뜀

        for (const auto& [group_name, group_ips] : rdma_groups) {
            for (const auto& rdma_ip : group_ips) {
                int matched_port = 0;
                // 매칭되는 포트가 있으면 클라이언트 실행
                if (match_device_to_ip(context, rdma_ip, matched_port)) {
                    // 클라이언트 실행 스레드 추가
                    client_threads.emplace_back(std::make_pair(
                        std::thread(run_client, context, matched_port, rdma_ip,
                                    group_ips),
                        context));
                }
            }
        }
    }

    // 모든 클라이언트 스레드가 종료될 때까지 대기
    for (auto& [thread, context] : client_threads) {
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
